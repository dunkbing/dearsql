#include "database/sqlite.hpp"
#include "database/ddl_builder.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <memory>
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

// Async table loading
void SQLiteDatabase::checkTablesStatusAsync() {
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tables = tablesFuture.get();
            Logger::info(std::format("Table loading completed. Found {} tables", tables.size()));
            tablesLoaded = true;
            loadingTables = false;
        } catch (const std::exception& e) {
            Logger::error(std::format("Error in table loading: {}", e.what()));
            lastTablesError = e.what();
            tablesLoaded = true;
            loadingTables = false;
        }
    }
}

void SQLiteDatabase::startTablesLoadAsync(bool forceRefresh) {
    Logger::debug("startTablesLoadAsync for SQLite database" +
                  std::string(forceRefresh ? " (force refresh)" : ""));

    if (loadingTables.load()) {
        return;
    }

    if (forceRefresh) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }

    if (!forceRefresh && tablesLoaded) {
        return;
    }

    loadingTables = true;
    tables.clear();

    tablesFuture = std::async(std::launch::async, [this]() { return getTablesAsync(); });
}

std::vector<Table> SQLiteDatabase::getTablesAsync() const {
    std::vector<Table> result;

    if (!loadingTables.load()) {
        return result;
    }

    try {
        if (!connected || !db_) {
            Logger::error("Database not connected");
            return result;
        }

        // Get table names
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery =
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'";
        queryRows(db_, tableNamesQuery, [&](sqlite3_stmt* stmt) {
            if (!loadingTables.load())
                return;
            tableNames.push_back(columnText(stmt, 0));
        });

        Logger::debug("Found " + std::to_string(tableNames.size()) + " tables in database");

        if (tableNames.empty() || !loadingTables.load()) {
            return result;
        }

        // Load table details
        for (const auto& tableName : tableNames) {
            if (!loadingTables.load()) {
                break;
            }

            Table table;
            table.name = tableName;
            table.fullName = connectionInfo.name + "." + tableName;

            // Get table columns
            const std::string columnsQuery = std::format("PRAGMA table_info({})", tableName);
            queryRows(db_, columnsQuery, [&](sqlite3_stmt* stmt) {
                Column col;
                col.name = columnText(stmt, 1);
                col.type = columnText(stmt, 2);
                col.isNotNull = columnText(stmt, 3) == "1";
                col.isPrimaryKey = columnText(stmt, 5) == "1";
                table.columns.push_back(std::move(col));
            });

            // Get foreign keys
            const std::string fkQuery = std::format("PRAGMA foreign_key_list({})", tableName);
            queryRows(db_, fkQuery, [&](sqlite3_stmt* stmt) {
                ForeignKey fk;
                fk.name = "";
                fk.targetTable = columnText(stmt, 2);
                fk.sourceColumn = columnText(stmt, 3);
                fk.targetColumn = columnText(stmt, 4);
                table.foreignKeys.push_back(std::move(fk));
            });

            // Get indexes
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

        Logger::info("Finished loading tables. Total tables: " + std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading tables: {}", e.what()));
    }

    return result;
}

// Async view loading
void SQLiteDatabase::checkViewsStatusAsync() {
    if (viewsFuture.valid() &&
        viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            views = viewsFuture.get();
            Logger::info(std::format("View loading completed. Found {} views", views.size()));
            viewsLoaded = true;
            loadingViews = false;
        } catch (const std::exception& e) {
            Logger::error(std::format("Error in view loading: {}", e.what()));
            lastViewsError = e.what();
            viewsLoaded = true;
            loadingViews = false;
        }
    }
}

void SQLiteDatabase::startViewsLoadAsync(bool forceRefresh) {
    Logger::debug("startViewsLoadAsync for SQLite database" +
                  std::string(forceRefresh ? " (force refresh)" : ""));

    if (loadingViews.load()) {
        return;
    }

    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    if (!forceRefresh && viewsLoaded) {
        return;
    }

    loadingViews = true;
    views.clear();

    viewsFuture = std::async(std::launch::async, [this]() { return getViewsAsync(); });
}

std::vector<Table> SQLiteDatabase::getViewsAsync() const {
    std::vector<Table> result;

    if (!loadingViews.load()) {
        return result;
    }

    try {
        if (!connected || !db_) {
            Logger::error("Database not connected");
            return result;
        }

        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SELECT name FROM sqlite_master WHERE type='view'";
        queryRows(db_, viewNamesQuery, [&](sqlite3_stmt* stmt) {
            if (!loadingViews.load())
                return;
            viewNames.push_back(columnText(stmt, 0));
        });

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in database");

        for (const auto& viewName : viewNames) {
            if (!loadingViews.load()) {
                break;
            }

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

        Logger::info("Finished loading views. Total views: " + std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading views: {}", e.what()));
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

std::vector<QueryResult> SQLiteDatabase::executeQueryWithResult(const std::string& query,
                                                                int rowLimit) {
    std::vector<QueryResult> results;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connected || !db_) {
        QueryResult r;
        r.success = false;
        r.errorMessage = "Database not connected";
        results.push_back(r);
        return results;
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
            QueryResult r;
            r.success = false;
            r.errorMessage = sqlite3_errmsg(db_);
            results.push_back(r);
            break;
        }

        if (!raw) {
            remaining = tail;
            continue;
        }

        StmtPtr stmt(raw);
        QueryResult r;

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
            r.success = true;
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            // DML/DDL statement
            rc = sqlite3_step(raw);
            if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
                r.success = true;
                r.affectedRows = sqlite3_changes(db_);
                r.message = "Query executed successfully";
            } else {
                r.success = false;
                r.errorMessage = sqlite3_errmsg(db_);
            }
        }

        results.push_back(std::move(r));
        remaining = tail;
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    auto totalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    if (!results.empty()) {
        results.back().executionTimeMs = totalMs;
    }

    return results;
}

std::pair<bool, std::string> SQLiteDatabase::executeQuery(const std::string& query) {
    if (!connected || !db_) {
        return {false, "Database not connected"};
    }
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, query.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string error = errmsg ? errmsg : "Unknown error";
        sqlite3_free(errmsg);
        return {false, error};
    }
    return {true, ""};
}

std::pair<bool, std::string> SQLiteDatabase::createTable(const Table& table) {
    if (!connected || !db_) {
        return {false, "Database not connected"};
    }

    try {
        DDLBuilder builder(DatabaseType::SQLITE);
        std::string sql = builder.createTable(table);
        return executeQuery(sql);
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
    checkTablesStatusAsync();
    checkViewsStatusAsync();
}

void SQLiteDatabase::startTableRefreshAsync(const std::string& tableName) {
    if (refreshingTables.contains(tableName)) {
        return;
    }

    refreshingTables.insert(tableName);
    tableRefreshFutures[tableName] = std::async(std::launch::async, [this, tableName]() {
        Table refreshedTable;
        refreshedTable.name = tableName;
        refreshedTable.fullName = connectionInfo.name + "." + tableName;

        try {
            // Get columns
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
            Logger::error(std::format("Error refreshing table {}: {}", tableName, e.what()));
        }

        return refreshedTable;
    });
}

bool SQLiteDatabase::isTableRefreshing(const std::string& tableName) const {
    return refreshingTables.contains(tableName);
}

void SQLiteDatabase::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshFutures.find(tableName);
    if (it == tableRefreshFutures.end()) {
        return;
    }

    if (it->second.valid() &&
        it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            Table refreshedTable = it->second.get();

            auto tableIt = std::find_if(tables.begin(), tables.end(), [&tableName](const Table& t) {
                return t.name == tableName;
            });
            if (tableIt != tables.end()) {
                *tableIt = refreshedTable;
                Logger::info(std::format("Table {} refreshed successfully", tableName));
            }

        } catch (const std::exception& e) {
            Logger::error(
                std::format("Error completing table refresh for {}: {}", tableName, e.what()));
        }

        refreshingTables.erase(tableName);
        tableRefreshFutures.erase(it);
    }
}
