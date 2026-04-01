#include "database/sqlite.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <utility>

namespace {
    // RAII wrapper for sqlite3_stmt
    struct StmtDeleter {
        void operator()(sqlite3_stmt* stmt) const {
            if (stmt)
                sqlite3_finalize(stmt);
        }
    };
    using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

    // Helper to get column value as string
    std::string columnText(sqlite3_stmt* stmt, int col) {
        if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
            return "NULL";
        }
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return text ? text : "";
    }

    // Helper to execute a query and iterate rows with a callback
    template <typename RowCallback>
    void queryRows(sqlite3* db, const std::string& sql, RowCallback&& callback) {
        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
        StmtPtr stmt(raw);
        while (sqlite3_step(raw) == SQLITE_ROW) {
            callback(raw);
        }
    }

    // Helper to get a single integer result
    int queryInt(sqlite3* db, const std::string& sql) {
        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
        StmtPtr stmt(raw);
        if (sqlite3_step(raw) == SQLITE_ROW) {
            return sqlite3_column_int(raw, 0);
        }
        return 0;
    }
} // namespace

SQLiteDatabase::SQLiteDatabase(const DatabaseConnectionInfo& connInfo) {
    connectionInfo = connInfo;
}

SQLiteDatabase::~SQLiteDatabase() {
    SQLiteDatabase::disconnect();
}

std::pair<bool, std::string> SQLiteDatabase::connect() {
    if (connected && db_) {
        return {true, ""};
    }

    int rc = sqlite3_open_v2(connectionInfo.path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "Unable to open database";
        std::cerr << "Can't open database: " << error << std::endl;
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return {false, error};
    }

    std::cout << "Successfully connected to database: " << connectionInfo.path << std::endl;
    connected = true;
    return {true, ""};
}

void SQLiteDatabase::disconnect() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    connected = false;
}

const std::string& SQLiteDatabase::getPath() const {
    return connectionInfo.path;
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
SQLiteDatabase::executeQueryStructured(const std::string& query, const int rowLimit) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!connected || !db_) {
        return {columnNames, data};
    }

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "executeQueryStructured error: " << sqlite3_errmsg(db_) << std::endl;
        return {columnNames, data};
    }
    StmtPtr stmt(raw);

    int colCount = sqlite3_column_count(raw);
    for (int i = 0; i < colCount; ++i) {
        columnNames.emplace_back(sqlite3_column_name(raw, i));
    }

    int rowCount = 0;
    while (sqlite3_step(raw) == SQLITE_ROW && rowCount < rowLimit) {
        std::vector<std::string> rowData;
        rowData.reserve(colCount);
        for (int i = 0; i < colCount; ++i) {
            rowData.push_back(columnText(raw, i));
        }
        data.push_back(std::move(rowData));
        ++rowCount;
    }

    return {columnNames, data};
}

std::vector<std::string> SQLiteDatabase::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;
    if (!connected || !db_) {
        return columnNames;
    }

    try {
        const std::string sql = "PRAGMA table_info(" + tableName + ");";
        queryRows(db_, sql,
                  [&](sqlite3_stmt* stmt) { columnNames.emplace_back(columnText(stmt, 1)); });
    } catch (const std::exception& e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }
    return columnNames;
}

// DatabaseInterface version (without whereClause) - delegates to ITableDataProvider version
std::vector<std::vector<std::string>> SQLiteDatabase::getTableData(const std::string& tableName,
                                                                   int limit, int offset) {
    return getTableData(tableName, limit, offset, "");
}

std::vector<std::string> SQLiteDatabase::getTableNames() const {
    std::vector<std::string> tableNames;

    std::cout << "Executing query to get table names..." << std::endl;
    try {
        const auto sql = "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;";
        queryRows(db_, sql, [&](sqlite3_stmt* stmt) {
            auto name = columnText(stmt, 0);
            std::cout << "Found table: " << name << std::endl;
            tableNames.push_back(std::move(name));
        });
    } catch (const std::exception& e) {
        std::cerr << "Failed to execute SQL statement: " << e.what() << std::endl;
    }
    std::cout << "Query completed. Found " << tableNames.size() << " tables." << std::endl;
    return tableNames;
}

std::vector<Index> SQLiteDatabase::getTableIndexes(const std::string& tableName) const {
    std::vector<Index> indexes;

    try {
        const std::string indexListSql = std::format("PRAGMA index_list('{}');", tableName);
        queryRows(db_, indexListSql, [&](sqlite3_stmt* stmt) {
            Index idx;
            idx.name = columnText(stmt, 1);

            std::string uniqueStr = columnText(stmt, 2);
            idx.isUnique = (uniqueStr == "1" || uniqueStr == "true");

            if (idx.name.find("sqlite_autoindex") != std::string::npos) {
                idx.isPrimary = true;
            }

            // Get columns for this index
            const std::string indexInfoSql = std::format("PRAGMA index_info('{}');", idx.name);
            queryRows(db_, indexInfoSql, [&](sqlite3_stmt* infoStmt) {
                idx.columns.push_back(columnText(infoStmt, 2));
            });

            idx.type = "BTREE";
            indexes.push_back(std::move(idx));
        });
    } catch (const std::exception& e) {
        std::cerr << "Error getting table indexes: " << e.what() << std::endl;
    }

    return indexes;
}

std::vector<ForeignKey> SQLiteDatabase::getTableForeignKeys(const std::string& tableName) const {
    std::vector<ForeignKey> foreignKeys;

    try {
        const std::string fkSql = std::format("PRAGMA foreign_key_list('{}');", tableName);
        queryRows(db_, fkSql, [&](sqlite3_stmt* stmt) {
            ForeignKey fk;
            fk.targetTable = columnText(stmt, 2);
            fk.sourceColumn = columnText(stmt, 3);
            fk.targetColumn = columnText(stmt, 4);
            fk.onUpdate = columnText(stmt, 5);
            fk.onDelete = columnText(stmt, 6);
            fk.name = std::format("fk_{}_{}", tableName, fk.sourceColumn);
            foreignKeys.push_back(std::move(fk));
        });
    } catch (const std::exception& e) {
        std::cerr << "Error getting table foreign keys: " << e.what() << std::endl;
    }

    return foreignKeys;
}

void SQLiteDatabase::startTablesLoadAsync(bool forceRefresh) {
    spdlog::debug("startTablesLoadAsync for SQLite database{}",
                  (forceRefresh ? " (force refresh)" : ""));

    if (forceRefresh) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }

    if (!forceRefresh && tablesLoaded) {
        return;
    }

    tables.clear();
    tablesLoader.start([this]() { return getTablesAsync(); });
}

std::vector<Table> SQLiteDatabase::getTablesAsync() const {
    std::vector<Table> result;

    try {
        if (!connected || !db_) {
            spdlog::error("Database not connected");
            return result;
        }

        std::vector<std::string> tableNames;
        const std::string tableNamesQuery =
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'";
        queryRows(db_, tableNamesQuery,
                  [&](sqlite3_stmt* stmt) { tableNames.push_back(columnText(stmt, 0)); });

        spdlog::debug("Found {} tables in database", tableNames.size());

        for (const auto& tableName : tableNames) {
            Table table;
            table.name = tableName;
            table.fullName = connectionInfo.name + "." + tableName;

            const std::string columnsQuery = std::format("PRAGMA table_info({})", tableName);
            queryRows(db_, columnsQuery, [&](sqlite3_stmt* stmt) {
                Column col;
                col.name = columnText(stmt, 1);
                col.type = columnText(stmt, 2);
                col.isNotNull = columnText(stmt, 3) == "1";
                col.isPrimaryKey = columnText(stmt, 5) == "1";
                table.columns.push_back(std::move(col));
            });

            const std::string fkQuery = std::format("PRAGMA foreign_key_list({})", tableName);
            queryRows(db_, fkQuery, [&](sqlite3_stmt* stmt) {
                ForeignKey fk;
                fk.name = "";
                fk.targetTable = columnText(stmt, 2);
                fk.sourceColumn = columnText(stmt, 3);
                fk.targetColumn = columnText(stmt, 4);
                table.foreignKeys.push_back(std::move(fk));
            });

            const std::string indexQuery = std::format("PRAGMA index_list({})", tableName);
            queryRows(db_, indexQuery, [&](sqlite3_stmt* stmt) {
                Index idx;
                idx.name = columnText(stmt, 1);
                idx.isUnique = columnText(stmt, 2) == "1";

                const std::string idxInfoQuery = std::format("PRAGMA index_info({})", idx.name);
                queryRows(db_, idxInfoQuery, [&](sqlite3_stmt* infoStmt) {
                    idx.columns.push_back(columnText(infoStmt, 2));
                });

                table.indexes.push_back(std::move(idx));
            });

            buildForeignKeyLookup(table);
            result.push_back(std::move(table));
        }

        populateIncomingForeignKeys(result);

        spdlog::debug("Finished loading tables. Total tables: {}", result.size());
    } catch (const std::exception& e) {
        spdlog::error("Error loading tables: {}", e.what());
    }

    return result;
}

void SQLiteDatabase::startViewsLoadAsync(bool forceRefresh) {
    spdlog::debug("startViewsLoadAsync for SQLite database{}",
                  (forceRefresh ? " (force refresh)" : ""));

    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    if (!forceRefresh && viewsLoaded) {
        return;
    }

    views.clear();
    viewsLoader.start([this]() { return getViewsAsync(); });
}

std::vector<Table> SQLiteDatabase::getViewsAsync() const {
    std::vector<Table> result;

    try {
        if (!connected || !db_) {
            spdlog::error("Database not connected");
            return result;
        }

        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SELECT name FROM sqlite_master WHERE type='view'";
        queryRows(db_, viewNamesQuery,
                  [&](sqlite3_stmt* stmt) { viewNames.push_back(columnText(stmt, 0)); });

        spdlog::debug("Found {} views in database", viewNames.size());

        for (const auto& viewName : viewNames) {
            Table view;
            view.name = viewName;
            view.fullName = connectionInfo.name + "." + viewName;

            const std::string columnsQuery = std::format("PRAGMA table_info({})", viewName);
            queryRows(db_, columnsQuery, [&](sqlite3_stmt* stmt) {
                Column col;
                col.name = columnText(stmt, 1);
                col.type = columnText(stmt, 2);
                col.isNotNull = columnText(stmt, 3) == "1";
                col.isPrimaryKey = false;
                view.columns.push_back(std::move(col));
            });

            result.push_back(std::move(view));
        }

        spdlog::debug("Finished loading views. Total views: {}", result.size());
    } catch (const std::exception& e) {
        spdlog::error("Error loading views: {}", e.what());
    }

    return result;
}

// ITableDataProvider implementation
std::vector<std::vector<std::string>>
SQLiteDatabase::getTableData(const std::string& tableName, int limit, int offset,
                             const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> data;
    if (!connected || !db_) {
        return data;
    }

    try {
        std::string sql = std::format("SELECT * FROM {}", tableName);
        if (!whereClause.empty()) {
            sql += std::format(" WHERE {}", whereClause);
        }
        if (!orderByClause.empty()) {
            sql += std::format(" ORDER BY {}", orderByClause);
        }
        sql += std::format(" LIMIT {} OFFSET {}", limit, offset);

        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Error getting table data: " << sqlite3_errmsg(db_) << std::endl;
            return data;
        }
        StmtPtr stmt(raw);

        int colCount = sqlite3_column_count(raw);
        while (sqlite3_step(raw) == SQLITE_ROW) {
            std::vector<std::string> rowData;
            rowData.reserve(colCount);
            for (int i = 0; i < colCount; ++i) {
                rowData.push_back(columnText(raw, i));
            }
            data.push_back(std::move(rowData));
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }
    return data;
}

int SQLiteDatabase::getRowCount(const std::string& tableName, const std::string& whereClause) {
    if (!connected || !db_) {
        return 0;
    }

    try {
        std::string sql;
        if (whereClause.empty()) {
            sql = "SELECT COUNT(*) FROM " + tableName;
        } else {
            sql = std::format("SELECT COUNT(*) FROM {} WHERE {}", tableName, whereClause);
        }
        return queryInt(db_, sql);
    } catch (const std::exception& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

QueryResult SQLiteDatabase::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connected || !db_) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Database not connected";
        result.statements.push_back(r);
        return result;
    }

    const char* remaining = query.c_str();
    while (remaining && *remaining) {
        // Skip whitespace
        while (*remaining && (*remaining == ' ' || *remaining == '\n' || *remaining == '\r' ||
                              *remaining == '\t')) {
            ++remaining;
        }
        if (!*remaining)
            break;

        sqlite3_stmt* raw = nullptr;
        const char* tail = nullptr;
        int rc = sqlite3_prepare_v2(db_, remaining, -1, &raw, &tail);

        if (rc != SQLITE_OK) {
            StatementResult r;
            r.success = false;
            r.errorMessage = sqlite3_errmsg(db_);
            result.statements.push_back(r);
            break;
        }

        if (!raw) {
            remaining = tail;
            continue;
        }

        StmtPtr stmt(raw);
        StatementResult r;

        int colCount = sqlite3_column_count(raw);
        if (colCount > 0) {
            // SELECT-like statement
            for (int i = 0; i < colCount; ++i) {
                r.columnNames.emplace_back(sqlite3_column_name(raw, i));
            }

            int rowCount = 0;
            while (sqlite3_step(raw) == SQLITE_ROW && rowCount < rowLimit) {
                std::vector<std::string> rowData;
                rowData.reserve(colCount);
                for (int i = 0; i < colCount; ++i) {
                    rowData.push_back(columnText(raw, i));
                }
                r.tableData.push_back(std::move(rowData));
                ++rowCount;
            }
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            // DML/DDL statement
            rc = sqlite3_step(raw);
            if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
                r.affectedRows = sqlite3_changes(db_);
                r.message = "Query executed successfully";
            } else {
                r.success = false;
                r.errorMessage = sqlite3_errmsg(db_);
            }
        }

        result.statements.push_back(std::move(r));
        remaining = tail;
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    return result;
}

std::pair<bool, std::string> SQLiteDatabase::createTable(const Table& table) {
    if (!connected || !db_) {
        return {false, "Database not connected"};
    }

    try {
        const auto builder = createSQLBuilder(DatabaseType::SQLITE);
        std::string sql = builder->createTable(table);
        auto result = executeQuery(sql);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

sqlite3* SQLiteDatabase::getSession() const {
    if (!connected || !db_) {
        return nullptr;
    }
    return db_;
}

std::string SQLiteDatabase::getName() const {
    const auto& path = connectionInfo.path;
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::string SQLiteDatabase::getFullPath() const {
    return connectionInfo.path;
}

void SQLiteDatabase::checkLoadingStatus() {
    tablesLoader.check([this](std::vector<Table> result) {
        tables = std::move(result);
        tablesLoaded = true;
        spdlog::debug("Table loading completed. Found {} tables", tables.size());
    });
    viewsLoader.check([this](std::vector<Table> result) {
        views = std::move(result);
        viewsLoaded = true;
        spdlog::debug("View loading completed. Found {} views", views.size());
    });
    for (auto it = tableRefreshLoaders.begin(); it != tableRefreshLoaders.end();) {
        const auto& tableName = it->first;
        it->second.check([this, &tableName](Table refreshedTable) {
            auto tableIt = std::find_if(tables.begin(), tables.end(), [&tableName](const Table& t) {
                return t.name == tableName;
            });
            if (tableIt != tables.end()) {
                *tableIt = std::move(refreshedTable);
                spdlog::debug("Table {} refreshed successfully", tableName);
            }
        });
        if (!it->second.isRunning()) {
            it = tableRefreshLoaders.erase(it);
        } else {
            ++it;
        }
    }
}

void SQLiteDatabase::startTableRefreshAsync(const std::string& tableName) {
    auto& loader = tableRefreshLoaders[tableName];
    loader.start([this, tableName]() {
        Table refreshedTable;
        refreshedTable.name = tableName;
        refreshedTable.fullName = connectionInfo.name + "." + tableName;

        try {
            const std::string columnsQuery = std::format("PRAGMA table_info(\"{}\")", tableName);
            queryRows(db_, columnsQuery, [&](sqlite3_stmt* stmt) {
                Column col;
                col.name = columnText(stmt, 1);
                col.type = columnText(stmt, 2);
                col.isNotNull = sqlite3_column_int(stmt, 3) != 0;
                col.isPrimaryKey = sqlite3_column_int(stmt, 5) != 0;
                refreshedTable.columns.push_back(std::move(col));
            });

            refreshedTable.indexes = getTableIndexes(tableName);
            refreshedTable.foreignKeys = getTableForeignKeys(tableName);
            buildForeignKeyLookup(refreshedTable);

        } catch (const std::exception& e) {
            spdlog::error("Error refreshing table {}: {}", tableName, e.what());
        }

        return refreshedTable;
    });
}

bool SQLiteDatabase::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

void SQLiteDatabase::checkTableRefreshStatusAsync(const std::string& tableName) {
    // handled by checkLoadingStatus
}

std::pair<bool, std::string> SQLiteDatabase::renameTable(const std::string& oldName,
                                                         const std::string& newName) {
    auto sql = std::format(R"(ALTER TABLE "{}" RENAME TO "{}")", oldName, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> SQLiteDatabase::dropTable(const std::string& tableName) {
    auto sql = std::format(R"(DROP TABLE "{}")", tableName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> SQLiteDatabase::dropColumn(const std::string& tableName,
                                                        const std::string& columnName) {
    auto sql = std::format(R"(ALTER TABLE "{}" DROP COLUMN "{}")", tableName, columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
