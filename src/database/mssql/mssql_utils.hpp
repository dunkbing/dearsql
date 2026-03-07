#pragma once

// shared utilities for MSSQL backend (internal header, not part of public API)

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <format>
#include <string>
#include <sybdb.h>

// thread-local error string captured by db-lib error/message callbacks
extern thread_local std::string tls_lastError;

inline void clearLastError() {
    tls_lastError.clear();
}

inline std::string getLastError() {
    return tls_lastError.empty() ? "Unknown error" : tls_lastError;
}

// convert a column value to string
inline std::string colToString(DBPROCESS* dbproc, int col) {
    BYTE* data = dbdata(dbproc, col);
    int len = dbdatlen(dbproc, col);
    if (!data || len < 0)
        return "NULL";

    int type = dbcoltype(dbproc, col);

    char buf[8192];
    DBINT converted =
        dbconvert(dbproc, type, data, len, SYBCHAR, reinterpret_cast<BYTE*>(buf), sizeof(buf) - 1);
    if (converted >= 0) {
        buf[converted] = '\0';
        return std::string(buf);
    }

    // fallback: raw bytes as string
    return std::string(reinterpret_cast<char*>(data), len);
}

// convert a column value to int
inline int colToInt(DBPROCESS* dbproc, int col) {
    BYTE* data = dbdata(dbproc, col);
    int len = dbdatlen(dbproc, col);
    if (!data || len < 0)
        return 0;

    int type = dbcoltype(dbproc, col);
    DBINT val = 0;
    dbconvert(dbproc, type, data, len, SYBINT4, reinterpret_cast<BYTE*>(&val), sizeof(val));
    return static_cast<int>(val);
}

// extract a result set from a DBPROCESS into a StatementResult
inline StatementResult extractDbLibResult(DBPROCESS* dbproc, int rowLimit) {
    StatementResult result;
    int numCols = dbnumcols(dbproc);

    if (numCols > 0) {
        for (int i = 1; i <= numCols; i++) {
            result.columnNames.emplace_back(dbcolname(dbproc, i));
        }

        int rowCount = 0;
        STATUS rowCode;
        while ((rowCode = dbnextrow(dbproc)) != NO_MORE_ROWS && rowCount < rowLimit) {
            if (rowCode == FAIL)
                break;
            std::vector<std::string> rowData;
            rowData.reserve(numCols);
            for (int i = 1; i <= numCols; i++) {
                rowData.push_back(colToString(dbproc, i));
            }
            result.tableData.push_back(std::move(rowData));
            rowCount++;
        }

        // drain any remaining rows past the limit so db-lib state stays clean
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
        }

        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        if (static_cast<int>(result.tableData.size()) >= rowLimit) {
            result.message += std::format(" (limited to {})", rowLimit);
        }
        result.success = true;
    } else {
        DBINT affected = DBCOUNT(dbproc);
        if (affected >= 0) {
            result.message = std::format("{} row(s) affected", affected);
        } else {
            result.message = "Query executed successfully";
        }
        result.success = true;
    }
    return result;
}

// execute a query on a DBPROCESS
inline bool execQuery(DBPROCESS* dbproc, const std::string& sql) {
    clearLastError();
    dbcmd(dbproc, sql.c_str());
    return dbsqlexec(dbproc) == SUCCEED;
}

// consume all remaining result sets
inline void drainResults(DBPROCESS* dbproc) {
    while (dbresults(dbproc) != NO_MORE_RESULTS) {
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
        }
    }
}

// split "schema.table" into {schema, table}, defaulting schema to "dbo"
inline std::pair<std::string, std::string> splitSchemaTable(const std::string& tableName) {
    auto dotPos = tableName.find('.');
    if (dotPos != std::string::npos) {
        return {tableName.substr(0, dotPos), tableName.substr(dotPos + 1)};
    }
    return {"dbo", tableName};
}

// bracket-quote a possibly schema-qualified table name: "schema.table" -> "[schema].[table]"
inline std::string quoteTableName(const std::string& tableName) {
    auto dotPos = tableName.find('.');
    if (dotPos != std::string::npos) {
        return std::format("[{}].[{}]", tableName.substr(0, dotPos), tableName.substr(dotPos + 1));
    }
    return std::format("[{}]", tableName);
}

// open a DBPROCESS connection using the given connection info
inline DBPROCESS* openDbLibConnection(const DatabaseConnectionInfo& info,
                                      const std::string& dbName = "") {
    LOGINREC* login = dblogin();
    if (!login)
        throw std::runtime_error("dblogin() failed");

    DBSETLUSER(login, info.username.c_str());
    DBSETLPWD(login, info.password.c_str());
    DBSETLAPP(login, "DearSQL");
    dbsetlversion(login, DBVERSION_73);

    if (info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
        info.sslmode == SslMode::VerifyFull) {
        DBSETLENCRYPT(login, TRUE);
    }

    dbsetlogintime(10);

    std::string serverStr = info.host + ":" + std::to_string(info.port);

    clearLastError();
    DBPROCESS* dbproc = dbopen(login, serverStr.c_str());
    dbloginfree(login);

    if (!dbproc) {
        throw std::runtime_error("MSSQL connection failed: " + getLastError());
    }

    const std::string targetDb = !dbName.empty() ? dbName : info.database;
    if (!targetDb.empty()) {
        clearLastError();
        if (dbuse(dbproc, targetDb.c_str()) != SUCCEED) {
            std::string err = getLastError();
            dbclose(dbproc);
            throw std::runtime_error("MSSQL dbuse failed: " + err);
        }
    }

    return dbproc;
}

// execute a query on a DBPROCESS and collect all result sets into a QueryResult
inline QueryResult executeQueryOnProcess(DBPROCESS* dbproc, const std::string& query,
                                         int rowLimit) {
    QueryResult result;

    clearLastError();
    dbcmd(dbproc, query.c_str());

    if (dbsqlexec(dbproc) == FAIL) {
        StatementResult r;
        r.success = false;
        r.errorMessage = getLastError();
        result.statements.push_back(r);
        dbcancel(dbproc);
        return result;
    }

    RETCODE rc;
    while ((rc = dbresults(dbproc)) != NO_MORE_RESULTS) {
        if (rc == FAIL) {
            StatementResult r;
            r.success = false;
            r.errorMessage = getLastError();
            result.statements.push_back(r);
            break;
        }
        auto r = extractDbLibResult(dbproc, rowLimit);
        if (r.success || !r.errorMessage.empty()) {
            result.statements.push_back(std::move(r));
        }
    }
    return result;
}

// RAII wrapper for a raw DBPROCESS* (use for temporary connections outside the pool)
struct DbProcessGuard {
    DBPROCESS* proc = nullptr;
    explicit DbProcessGuard(DBPROCESS* p) : proc(p) {}
    ~DbProcessGuard() {
        if (proc)
            dbclose(proc);
    }
    DbProcessGuard(const DbProcessGuard&) = delete;
    DbProcessGuard& operator=(const DbProcessGuard&) = delete;
    DBPROCESS* get() const {
        return proc;
    }
    DBPROCESS* release() {
        auto* p = proc;
        proc = nullptr;
        return p;
    }
};
