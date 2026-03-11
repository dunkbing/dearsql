#pragma once

// shared utilities for Oracle backend using ODPI-C (internal header)

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/oracle.hpp"
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <dpi.h>

// extract error message from ODPI-C context or connection
inline std::string dpiGetError(dpiContext* ctx) {
    dpiErrorInfo info;
    dpiContext_getError(ctx, &info);
    return std::string(info.message, info.messageLength);
}

inline std::string dpiGetConnError(dpiConn* conn) {
    return dpiGetError(OracleDatabase::getContext());
}

// convert a dpiData value to string
inline std::string dpiDataToString(dpiNativeTypeNum nativeType, dpiData* data) {
    if (data->isNull)
        return "NULL";

    switch (nativeType) {
    case DPI_NATIVE_TYPE_BYTES:
        return std::string(data->value.asBytes.ptr, data->value.asBytes.length);
    case DPI_NATIVE_TYPE_DOUBLE:
        return std::format("{}", data->value.asDouble);
    case DPI_NATIVE_TYPE_FLOAT:
        return std::format("{}", data->value.asFloat);
    case DPI_NATIVE_TYPE_INT64:
        return std::format("{}", data->value.asInt64);
    case DPI_NATIVE_TYPE_UINT64:
        return std::format("{}", data->value.asUint64);
    case DPI_NATIVE_TYPE_BOOLEAN:
        return data->value.asBoolean ? "TRUE" : "FALSE";
    case DPI_NATIVE_TYPE_TIMESTAMP: {
        auto& ts = data->value.asTimestamp;
        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", ts.year, ts.month, ts.day,
                           ts.hour, ts.minute, ts.second);
    }
    case DPI_NATIVE_TYPE_INTERVAL_DS: {
        auto& iv = data->value.asIntervalDS;
        return std::format("{} {:02d}:{:02d}:{:02d}", iv.days, iv.hours, iv.minutes, iv.seconds);
    }
    case DPI_NATIVE_TYPE_INTERVAL_YM: {
        auto& iv = data->value.asIntervalYM;
        return std::format("{}-{}", iv.years, iv.months);
    }
    default:
        return "<unsupported>";
    }
}

// execute a query and return a QueryResult
inline QueryResult dpiExecuteQuery(dpiConn* conn, const std::string& query, int rowLimit) {
    QueryResult result;
    dpiContext* ctx = OracleDatabase::getContext();

    dpiStmt* stmt = nullptr;
    if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()), nullptr, 0,
                            &stmt) != DPI_SUCCESS) {
        StatementResult r;
        r.success = false;
        r.errorMessage = dpiGetError(ctx);
        result.statements.push_back(r);
        return result;
    }

    uint32_t numQueryColumns = 0;
    int execMode = DPI_MODE_EXEC_DEFAULT;

    // detect statement type
    dpiStmtInfo stmtInfo;
    dpiStmt_getInfo(stmt, &stmtInfo);
    if (!stmtInfo.isQuery) {
        execMode = DPI_MODE_EXEC_COMMIT_ON_SUCCESS;
    }

    if (dpiStmt_execute(stmt, static_cast<dpiExecMode>(execMode), &numQueryColumns) !=
        DPI_SUCCESS) {
        StatementResult r;
        r.success = false;
        r.errorMessage = dpiGetError(ctx);
        result.statements.push_back(r);
        dpiStmt_release(stmt);
        return result;
    }

    StatementResult r;

    if (numQueryColumns > 0) {
        // SELECT — get column names
        for (uint32_t i = 1; i <= numQueryColumns; i++) {
            dpiQueryInfo queryInfo;
            dpiStmt_getQueryInfo(stmt, i, &queryInfo);
            r.columnNames.emplace_back(queryInfo.name, queryInfo.nameLength);
        }

        // fetch rows
        int rowCount = 0;
        int found = 0;
        uint32_t bufferRowIndex = 0;
        while (rowCount < rowLimit && dpiStmt_fetch(stmt, &found, &bufferRowIndex) == DPI_SUCCESS &&
               found) {
            std::vector<std::string> rowData;
            rowData.reserve(numQueryColumns);
            for (uint32_t i = 1; i <= numQueryColumns; i++) {
                dpiNativeTypeNum nativeType;
                dpiData* data;
                dpiStmt_getQueryValue(stmt, i, &nativeType, &data);
                rowData.push_back(dpiDataToString(nativeType, data));
            }
            r.tableData.push_back(std::move(rowData));
            rowCount++;
        }

        r.message = std::format("Returned {} row{}", r.tableData.size(),
                                r.tableData.size() == 1 ? "" : "s");
        if (static_cast<int>(r.tableData.size()) >= rowLimit) {
            r.message += std::format(" (limited to {})", rowLimit);
        }
        r.success = true;
    } else {
        // DML/DDL
        uint64_t rowsAffected = 0;
        dpiStmt_getRowCount(stmt, &rowsAffected);
        r.affectedRows = static_cast<int>(rowsAffected);
        if (rowsAffected > 0) {
            r.message = std::format("{} row(s) affected", rowsAffected);
        } else {
            r.message = "Query executed successfully";
        }
        r.success = true;
    }

    result.statements.push_back(std::move(r));
    dpiStmt_release(stmt);
    return result;
}

// execute a single-column query returning string list
inline std::vector<std::string> dpiQueryStringList(dpiConn* conn, const std::string& sql) {
    std::vector<std::string> results;
    dpiContext* ctx = OracleDatabase::getContext();

    dpiStmt* stmt = nullptr;
    if (dpiConn_prepareStmt(conn, 0, sql.c_str(), static_cast<uint32_t>(sql.size()), nullptr, 0,
                            &stmt) != DPI_SUCCESS) {
        return results;
    }

    uint32_t numCols = 0;
    if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS || numCols == 0) {
        dpiStmt_release(stmt);
        return results;
    }

    int found = 0;
    uint32_t bufIdx = 0;
    while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
        dpiNativeTypeNum nativeType;
        dpiData* data;
        dpiStmt_getQueryValue(stmt, 1, &nativeType, &data);
        if (!data->isNull) {
            results.push_back(dpiDataToString(nativeType, data));
        }
    }

    dpiStmt_release(stmt);
    return results;
}

// execute a scalar query (single value)
inline std::string dpiQueryScalar(dpiConn* conn, const std::string& sql) {
    auto list = dpiQueryStringList(conn, sql);
    return list.empty() ? "" : list[0];
}

inline std::string oracleWalletLocation(const std::string& path) {
    if (path.empty())
        return {};

    std::error_code ec;
    std::filesystem::path walletPath(path);
    if (std::filesystem::is_regular_file(walletPath, ec)) {
        auto parent = walletPath.parent_path();
        if (!parent.empty()) {
            return parent.string();
        }
    }
    return walletPath.string();
}

inline std::string buildOracleConnectString(const DatabaseConnectionInfo& info) {
    const bool useTls = info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
                        info.sslmode == SslMode::VerifyFull;
    const bool needsWallet =
        info.sslmode == SslMode::VerifyCA || info.sslmode == SslMode::VerifyFull;

    std::string connectString =
        useTls ? std::format("tcps://{}:{}/{}", info.host, info.port, info.database)
               : std::format("{}:{}/{}", info.host, info.port, info.database);

    if (!useTls) {
        return connectString;
    }

    std::vector<std::string> params;
    if (info.sslmode == SslMode::Require) {
        params.emplace_back("ssl_server_dn_match=off");
    }

    if (needsWallet) {
        auto walletLocation = oracleWalletLocation(info.sslCACertPath);
        if (walletLocation.empty()) {
            throw std::runtime_error(
                "Oracle TLS verify mode requires a wallet path or wallet file location");
        }
        params.push_back(std::format("wallet_location=\"{}\"", walletLocation));
    }

    if (!params.empty()) {
        connectString += '?';
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) {
                connectString += '&';
            }
            connectString += params[i];
        }
    }

    return connectString;
}

// open a new ODPI-C connection
inline dpiConn* openDpiConnection(const DatabaseConnectionInfo& info,
                                  const std::string& schema = "") {
    dpiContext* ctx = OracleDatabase::getContext();
    dpiConn* conn = nullptr;

    auto connStr = buildOracleConnectString(info);

    if (dpiConn_create(ctx, info.username.c_str(), static_cast<uint32_t>(info.username.size()),
                       info.password.c_str(), static_cast<uint32_t>(info.password.size()),
                       connStr.c_str(), static_cast<uint32_t>(connStr.size()), nullptr, nullptr,
                       &conn) != DPI_SUCCESS) {
        throw std::runtime_error("Oracle connection failed: " + dpiGetError(ctx));
    }

    // switch schema if specified
    if (!schema.empty()) {
        std::string alterSql = std::format("ALTER SESSION SET CURRENT_SCHEMA = \"{}\"", schema);
        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, alterSql.c_str(), static_cast<uint32_t>(alterSql.size()),
                                nullptr, 0, &stmt) == DPI_SUCCESS) {
            uint32_t numCols = 0;
            dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols);
            dpiStmt_release(stmt);
        }
    }

    return conn;
}

// close an ODPI-C connection
inline void closeDpiConnection(dpiConn* conn) {
    if (conn) {
        dpiConn_release(conn);
    }
}

// check if a connection is alive
inline bool dpiConnectionAlive(dpiConn* conn) {
    if (!conn)
        return false;
    return dpiConn_ping(conn) == DPI_SUCCESS;
}

// double-quote an Oracle identifier
inline std::string quoteOracleId(const std::string& id) {
    return std::format("\"{}\"", id);
}

// fully qualified table name: "SCHEMA"."TABLE"
inline std::string quoteOracleTable(const std::string& schema, const std::string& table) {
    return std::format("\"{}\".\"{}\"", schema, table);
}
