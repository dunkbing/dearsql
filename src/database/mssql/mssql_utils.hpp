#pragma once

// shared utilities for MSSQL backend (internal header, not part of public API)

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <sybdb.h>
#include <vector>

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
        return std::string(NULL_SENTINEL);

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

// escape a MSSQL identifier: double any embedded ]
inline std::string quoteMssqlId(const std::string& id) {
    std::string out = "[";
    out.reserve(id.size() + 2);
    for (char c : id) {
        if (c == ']')
            out += ']';
        out += c;
    }
    out += ']';
    return out;
}

// bracket-quote a possibly schema-qualified table name: "schema.table" -> "[schema].[table]"
inline std::string quoteTableName(const std::string& tableName) {
    auto dotPos = tableName.find('.');
    if (dotPos != std::string::npos) {
        return quoteMssqlId(tableName.substr(0, dotPos)) + "." +
               quoteMssqlId(tableName.substr(dotPos + 1));
    }
    return quoteMssqlId(tableName);
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

// load columns for a table/view
inline std::vector<Column> loadColumns(DBPROCESS* dbproc, const std::string& schema,
                                       const std::string& tableName) {
    std::vector<Column> columns;
    std::string query = std::format("SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, "
                                    "COLUMNPROPERTY(OBJECT_ID('{}.{}'), COLUMN_NAME, 'IsIdentity') "
                                    "FROM INFORMATION_SCHEMA.COLUMNS "
                                    "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                                    "AND TABLE_NAME = '{}' ORDER BY ORDINAL_POSITION",
                                    schema, tableName, schema, tableName);

    if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
            Column col;
            col.name = colToString(dbproc, 1);
            col.type = colToString(dbproc, 2);
            col.isNotNull = colToString(dbproc, 3) == "NO";
            col.isAutoIncrement = colToString(dbproc, 4) == "1";
            columns.push_back(col);
        }
    }
    drainResults(dbproc);
    return columns;
}

// mark primary key columns
inline void loadPrimaryKeys(DBPROCESS* dbproc, const std::string& schema,
                            const std::string& tableName, std::vector<Column>& columns) {
    std::string query = std::format("SELECT c.COLUMN_NAME "
                                    "FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                                    "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                                    "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                                    "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                                    "WHERE tc.TABLE_SCHEMA = '{}' AND tc.TABLE_NAME = '{}' "
                                    "AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY'",
                                    schema, tableName);

    if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
            std::string pkCol = colToString(dbproc, 1);
            for (auto& col : columns) {
                if (col.name == pkCol) {
                    col.isPrimaryKey = true;
                    break;
                }
            }
        }
    }
    drainResults(dbproc);
}

// load foreign keys for a table
inline std::vector<ForeignKey> loadForeignKeys(DBPROCESS* dbproc, const std::string& schema,
                                               const std::string& tableName) {
    std::vector<ForeignKey> fks;
    std::string query =
        std::format("SELECT fk.name, "
                    "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
                    "OBJECT_NAME(fkc.referenced_object_id), "
                    "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
                    "FROM sys.foreign_keys fk "
                    "JOIN sys.foreign_key_columns fkc ON fk.object_id = fkc.constraint_object_id "
                    "WHERE OBJECT_SCHEMA_NAME(fk.parent_object_id) = '{}' "
                    "AND OBJECT_NAME(fk.parent_object_id) = '{}'",
                    schema, tableName);

    if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
            ForeignKey fk;
            fk.name = colToString(dbproc, 1);
            fk.sourceColumn = colToString(dbproc, 2);
            fk.targetTable = colToString(dbproc, 3);
            fk.targetColumn = colToString(dbproc, 4);
            fks.push_back(fk);
        }
    }
    drainResults(dbproc);
    return fks;
}

// load indexes for a table
inline std::vector<Index> loadIndexes(DBPROCESS* dbproc, const std::string& schema,
                                      const std::string& tableName) {
    std::vector<Index> indexes;
    std::string query = std::format("SELECT i.name, i.is_unique, c.name AS column_name "
                                    "FROM sys.indexes i "
                                    "JOIN sys.index_columns ic ON i.object_id = ic.object_id "
                                    "AND i.index_id = ic.index_id "
                                    "JOIN sys.columns c ON ic.object_id = c.object_id "
                                    "AND ic.column_id = c.column_id "
                                    "WHERE i.object_id = OBJECT_ID('{}.{}') "
                                    "AND i.is_primary_key = 0 AND i.type > 0 "
                                    "ORDER BY i.name, ic.key_ordinal",
                                    schema, tableName);

    if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
        std::map<std::string, Index> indexMap;
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
            std::string idxName = colToString(dbproc, 1);
            int isUnique = colToInt(dbproc, 2);
            std::string colName = colToString(dbproc, 3);

            if (!indexMap.contains(idxName)) {
                Index idx;
                idx.name = idxName;
                idx.isUnique = (isUnique != 0);
                indexMap[idxName] = idx;
            }
            indexMap[idxName].columns.push_back(colName);
        }
        for (auto& idx : indexMap | std::views::values) {
            indexes.push_back(std::move(idx));
        }
    }
    drainResults(dbproc);
    return indexes;
}

// load all metadata for a single table
inline Table loadTableMetadata(DBPROCESS* dbproc, const std::string& schema,
                               const std::string& tableName, const std::string& displayName,
                               const std::string& fullName) {
    Table table;
    table.name = displayName;
    table.fullName = fullName;
    table.columns = loadColumns(dbproc, schema, tableName);
    loadPrimaryKeys(dbproc, schema, tableName, table.columns);
    table.foreignKeys = loadForeignKeys(dbproc, schema, tableName);
    table.indexes = loadIndexes(dbproc, schema, tableName);
    return table;
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
