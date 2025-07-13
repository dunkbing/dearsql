#include "database/query_executor.hpp"
#include <sstream>

std::string QueryExecutor::executeQuery(sqlite3 *db, const std::string &query) {
    std::stringstream result;
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return "Error: " + std::string(sqlite3_errmsg(db));
    }

    // Get column count and names
    const int columnCount = sqlite3_column_count(stmt);
    if (columnCount > 0) {
        // Headers
        for (int i = 0; i < columnCount; i++) {
            result << sqlite3_column_name(stmt, i);
            if (i < columnCount - 1)
                result << " | ";
        }
        result << "\n";

        // Separator
        for (int i = 0; i < columnCount; i++) {
            result << "----------";
            if (i < columnCount - 1)
                result << "-+-";
        }
        result << "\n";
    }

    // Data rows
    int rowCount = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && rowCount < 1000) { // Limit to 1000 rows
        for (int i = 0; i < columnCount; i++) {
            const char *text = (const char *)sqlite3_column_text(stmt, i);
            result << (text ? text : "NULL");
            if (i < columnCount - 1)
                result << " | ";
        }
        result << "\n";
        rowCount++;
    }

    if (rowCount == 0 && columnCount == 0) {
        result << "Query executed successfully. Rows affected: " << sqlite3_changes(db);
    } else if (rowCount == 1000) {
        result << "\n... (showing first 1000 rows)";
    }

    sqlite3_finalize(stmt);
    return result.str();
}

std::vector<std::vector<std::string>>
QueryExecutor::getTableData(sqlite3 *db, const std::string &tableName, int limit, int offset) {
    std::vector<std::vector<std::string>> data;
    const std::string sql = "SELECT * FROM " + tableName + " LIMIT " + std::to_string(limit) +
                            " OFFSET " + std::to_string(offset);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
        const int columnCount = sqlite3_column_count(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int i = 0; i < columnCount; i++) {
                auto text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
                row.emplace_back(text ? text : "NULL");
            }
            data.push_back(row);
        }
    }
    sqlite3_finalize(stmt);
    return data;
}

std::vector<std::string> QueryExecutor::getColumnNames(sqlite3 *db, const std::string &tableName) {
    std::vector<std::string> columnNames;
    std::string sql = "PRAGMA table_info(" + tableName + ");";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            columnNames.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)));
        }
    }
    sqlite3_finalize(stmt);
    return columnNames;
}

int QueryExecutor::getRowCount(sqlite3 *db, const std::string &tableName) {
    std::string sql = "SELECT COUNT(*) FROM " + tableName;
    sqlite3_stmt *stmt;
    int count = 0;

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return count;
}
