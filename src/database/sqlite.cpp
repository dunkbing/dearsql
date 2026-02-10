#include "database/sqlite.hpp"
#include "database/ddl_utils.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <memory>
#include <typeinfo>
#include <utility>

SQLiteDatabase::SQLiteDatabase(const DatabaseConnectionInfo& connInfo) {
    connectionInfo = connInfo;
}

SQLiteDatabase::~SQLiteDatabase() {
    SQLiteDatabase::disconnect();
}

std::pair<bool, std::string> SQLiteDatabase::connect() {
    if (connected && session) {
        return {true, ""};
    }

    try {
        session = std::make_unique<soci::session>(soci::sqlite3, connectionInfo.path);
        std::cout << "Successfully connected to database: " << connectionInfo.path << std::endl;
        connected = true;
        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::string error = e.what();
        std::cerr << "Can't open database: " << error << std::endl;
        return {false, error};
    }
}

void SQLiteDatabase::disconnect() {
    if (session) {
        session.reset();
    }
    connected = false;
}

const std::string& SQLiteDatabase::getPath() const {
    return connectionInfo.path;
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
SQLiteDatabase::executeQueryStructured(const std::string& query) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!connected) {
        return {columnNames, data};
    }

    try {
        const soci::rowset rs = session->prepare << query;

        // Get column names if available
        auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        int rowCount = 0;
        for (const auto& row : rs) {
            if (rowCount >= 1000)
                break;

            std::vector<std::string> rowData;
            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.push_back(convertRowValue(row, i));
            }
            data.push_back(rowData);
            rowCount++;
        }

        return {columnNames, data};
    } catch (const soci::soci_error& e) {
        // Return empty result on error
        std::cerr << "executeQueryStructured error: " << e.what() << std::endl;
        return {columnNames, data};
    }
}

std::vector<std::string> SQLiteDatabase::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;
    if (!connected) {
        return columnNames;
    }

    try {
        const std::string sql = "PRAGMA table_info(" + tableName + ");";
        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto& row : rs) {
            columnNames.emplace_back(row.get<std::string>(1));
        }
    } catch (const soci::soci_error& e) {
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
        const soci::rowset rs = session->prepare << sql;

        for (const auto& row : rs) {
            auto tableName = row.get<std::string>(0);
            std::cout << "Found table: " << tableName << std::endl;
            tableNames.emplace_back(tableName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute SQL statement: " << e.what() << std::endl;
    }
    std::cout << "Query completed. Found " << tableNames.size() << " tables." << std::endl;
    return tableNames;
}

std::vector<Index> SQLiteDatabase::getTableIndexes(const std::string& tableName) const {
    std::vector<Index> indexes;

    try {
        // Get list of indexes for the table
        const std::string indexListSql = std::format("PRAGMA index_list('{}');", tableName);
        const soci::rowset indexList = session->prepare << indexListSql;

        for (const auto& indexRow : indexList) {
            Index idx;
            idx.name = convertRowValue(indexRow, 1); // name

            // Handle unique column (can be int or string)
            std::string uniqueStr = convertRowValue(indexRow, 2);
            idx.isUnique = (uniqueStr == "1" || uniqueStr == "true");

            // Determine if this is a primary key index
            if (idx.name.find("sqlite_autoindex") != std::string::npos) {
                idx.isPrimary = true;
            }

            // Get columns for this index
            const std::string indexInfoSql = std::format("PRAGMA index_info('{}');", idx.name);
            const soci::rowset indexInfo = session->prepare << indexInfoSql;

            for (const auto& colRow : indexInfo) {
                std::string colName = convertRowValue(colRow, 2); // name
                idx.columns.push_back(colName);
            }

            // Default type for SQLite is BTREE
            idx.type = "BTREE";

            indexes.push_back(idx);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table indexes: " << e.what() << std::endl;
    }

    return indexes;
}

std::vector<ForeignKey> SQLiteDatabase::getTableForeignKeys(const std::string& tableName) const {
    std::vector<ForeignKey> foreignKeys;

    try {
        const std::string fkSql = std::format("PRAGMA foreign_key_list('{}');", tableName);
        const soci::rowset fkList = session->prepare << fkSql;

        for (const auto& fkRow : fkList) {
            ForeignKey fk;
            // SQLite PRAGMA foreign_key_list columns:
            // 0: id, 1: seq, 2: table (target), 3: from, 4: to, 5: on_update, 6: on_delete, 7:
            // match
            fk.targetTable = fkRow.get<std::string>(2);
            fk.sourceColumn = fkRow.get<std::string>(3);
            fk.targetColumn = fkRow.get<std::string>(4);
            fk.onUpdate = fkRow.get<std::string>(5);
            fk.onDelete = fkRow.get<std::string>(6);

            // Generate a name if not provided
            fk.name = std::format("fk_{}_{}", tableName, fk.sourceColumn);

            foreignKeys.push_back(fk);
        }
    } catch (const soci::soci_error& e) {
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

    // Don't start if already loading
    if (loadingTables.load()) {
        return;
    }

    // If force refresh, clear existing tables and reset state
    if (forceRefresh) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }

    // Don't start if already loaded (unless force refresh)
    if (!forceRefresh && tablesLoaded) {
        return;
    }

    loadingTables = true;
    tables.clear();

    // Start async loading
    tablesFuture = std::async(std::launch::async, [this]() { return getTablesAsync(); });
}

std::vector<Table> SQLiteDatabase::getTablesAsync() const {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingTables.load()) {
        return result;
    }

    try {
        // Check connection
        if (!connected) {
            Logger::error("Database not connected");
            return result;
        }

        // Get table names
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery =
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'";
        {
            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(convertRowValue(row, 0));
            }
        }

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
            {
                const soci::rowset colRs = session->prepare << columnsQuery;
                for (const auto& row : colRs) {
                    Column col;
                    col.name = convertRowValue(row, 1);                // name
                    col.type = convertRowValue(row, 2);                // type
                    col.isNotNull = convertRowValue(row, 3) == "1";    // notnull
                    col.isPrimaryKey = convertRowValue(row, 5) == "1"; // pk

                    table.columns.push_back(col);
                }
            }

            // Get foreign keys
            const std::string fkQuery = std::format("PRAGMA foreign_key_list({})", tableName);
            {
                const soci::rowset fkRs = session->prepare << fkQuery;
                for (const auto& row : fkRs) {
                    ForeignKey fk;
                    fk.name = "";                              // SQLite doesn't name FKs
                    fk.targetTable = convertRowValue(row, 2);  // table
                    fk.sourceColumn = convertRowValue(row, 3); // from
                    fk.targetColumn = convertRowValue(row, 4); // to

                    table.foreignKeys.push_back(fk);
                }
            }

            // Get indexes
            const std::string indexQuery = std::format("PRAGMA index_list({})", tableName);
            {
                const soci::rowset idxRs = session->prepare << indexQuery;
                for (const auto& row : idxRs) {
                    Index idx;
                    idx.name = convertRowValue(row, 1);            // name
                    idx.isUnique = convertRowValue(row, 2) == "1"; // unique

                    // Get index columns
                    const std::string idxInfoQuery = std::format("PRAGMA index_info({})", idx.name);
                    const soci::rowset idxInfoRs = session->prepare << idxInfoQuery;
                    for (const auto& infoRow : idxInfoRs) {
                        idx.columns.push_back(convertRowValue(infoRow, 2)); // name
                    }

                    table.indexes.push_back(idx);
                }
            }

            // Build foreign key lookup
            buildForeignKeyLookup(table);

            result.push_back(table);
        }

        // Populate incoming foreign keys
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

    // Don't start if already loading
    if (loadingViews.load()) {
        return;
    }

    // If force refresh, clear existing views and reset state
    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    // Don't start if already loaded (unless force refresh)
    if (!forceRefresh && viewsLoaded) {
        return;
    }

    loadingViews = true;
    views.clear();

    // Start async loading
    viewsFuture = std::async(std::launch::async, [this]() { return getViewsAsync(); });
}

std::vector<Table> SQLiteDatabase::getViewsAsync() const {
    std::vector<Table> result;

    if (!loadingViews.load()) {
        return result;
    }

    try {
        if (!connected) {
            Logger::error("Database not connected");
            return result;
        }

        // Get view names
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SELECT name FROM sqlite_master WHERE type='view'";
        {
            const soci::rowset viewRs = session->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(convertRowValue(row, 0));
            }
        }

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in database");

        // Load view details
        for (const auto& viewName : viewNames) {
            if (!loadingViews.load()) {
                break;
            }

            Table view;
            view.name = viewName;
            view.fullName = connectionInfo.name + "." + viewName;

            // Get view columns
            const std::string columnsQuery = std::format("PRAGMA table_info({})", viewName);
            {
                const soci::rowset colRs = session->prepare << columnsQuery;
                for (const auto& row : colRs) {
                    Column col;
                    col.name = convertRowValue(row, 1);             // name
                    col.type = convertRowValue(row, 2);             // type
                    col.isNotNull = convertRowValue(row, 3) == "1"; // notnull
                    col.isPrimaryKey = false;                       // views don't have PKs

                    view.columns.push_back(col);
                }
            }

            result.push_back(view);
        }

        Logger::info("Finished loading views. Total views: " + std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading views: {}", e.what()));
    }

    return result;
}

// ITableDataProvider implementation (with whereClause and orderByClause support)
std::vector<std::vector<std::string>>
SQLiteDatabase::getTableData(const std::string& tableName, int limit, int offset,
                             const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> data;
    if (!connected) {
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

        const soci::rowset rs = session->prepare << sql;

        for (const auto& row : rs) {
            std::vector<std::string> rowData;

            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                    continue;
                }
                soci::column_properties cp = row.get_properties(i);
                const auto dt = cp.get_db_type();
                switch (dt) {
                case soci::db_string:
                    rowData.emplace_back(row.get<std::string>(i));
                    break;
                case soci::db_wstring: {
                    auto ws = row.get<std::wstring>(i);
                    std::string utf8_str(ws.begin(), ws.end());
                    rowData.emplace_back(utf8_str);
                } break;
                case soci::db_int8:
                    rowData.emplace_back(std::to_string(row.get<int8_t>(i)));
                    break;
                case soci::db_int16:
                    rowData.emplace_back(std::to_string(row.get<int16_t>(i)));
                    break;
                case soci::db_int32:
                    rowData.emplace_back(std::to_string(row.get<int32_t>(i)));
                    break;
                case soci::db_int64:
                    rowData.emplace_back(std::to_string(row.get<int64_t>(i)));
                    break;
                case soci::db_double:
                    rowData.emplace_back(std::to_string(row.get<double>(i)));
                    break;
                case soci::db_blob:
                    rowData.emplace_back("[BINARY DATA]");
                    break;
                default:
                    try {
                        rowData.emplace_back(row.get<std::string>(i));
                    } catch (const std::bad_cast&) {
                        rowData.emplace_back("[UNKNOWN DATA TYPE]");
                    }
                    break;
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }
    return data;
}

int SQLiteDatabase::getRowCount(const std::string& tableName, const std::string& whereClause) {
    if (!connected) {
        return 0;
    }

    try {
        std::string sql;
        if (whereClause.empty()) {
            sql = "SELECT COUNT(*) FROM " + tableName;
        } else {
            sql = std::format("SELECT COUNT(*) FROM {} WHERE {}", tableName, whereClause);
        }
        int count = 0;
        *session << sql, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

QueryResult SQLiteDatabase::executeQueryWithResult(const std::string& query, int rowLimit) {
    QueryResult result;
    if (!connected) {
        result.success = false;
        result.message = "Error: Database not connected";
        return result;
    }

    try {
        auto [columns, data] = executeQueryStructured(query);
        result.success = true;
        result.columnNames = columns;
        result.tableData = data;
        result.errorMessage = "Query executed successfully";
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("Error: ") + e.what();
    }

    return result;
}

std::pair<bool, std::string> SQLiteDatabase::executeQuery(const std::string& query) {
    if (!connected || !session) {
        return {false, "Database not connected"};
    }
    try {
        *session << query;
        return {true, ""};
    } catch (const soci::soci_error& e) {
        return {false, std::string(e.what())};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

std::pair<bool, std::string> SQLiteDatabase::createTable(const Table& table) {
    if (!connected || !session) {
        return {false, "Database not connected"};
    }

    try {
        soci::ddl_type ddl = session->create_table(table.name);

        std::vector<std::string> primaryKeyColumns;
        for (const auto& column : table.columns) {
            const auto ddlType = ddl_utils::inferColumnType(column.type);
            auto& ddlRef = ddl.column(column.name, ddlType.type, ddlType.precision, ddlType.scale);

            if (column.isNotNull && !column.isPrimaryKey) {
                ddlRef("not null");
            }

            if (column.isPrimaryKey) {
                primaryKeyColumns.push_back(column.name);
            }
        }

        if (!primaryKeyColumns.empty()) {
            ddl.primary_key(ddl_utils::makeConstraintName("pk_", table.name),
                            ddl_utils::joinColumnNames(primaryKeyColumns));
        }

        return {true, ""};
    } catch (const soci::soci_error& e) {
        return {false, std::string(e.what())};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

soci::session* SQLiteDatabase::getSession() const {
    if (!connected || !session) {
        return nullptr;
    }
    return session.get();
}

std::string SQLiteDatabase::getName() const {
    // Return just the filename from the path
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
            const soci::rowset columnsRs = session->prepare << columnsQuery;
            for (const auto& row : columnsRs) {
                Column col;
                col.name = convertRowValue(row, 1);
                col.type = convertRowValue(row, 2);
                col.isNotNull = row.get<int>(3) != 0;
                col.isPrimaryKey = row.get<int>(5) != 0;
                refreshedTable.columns.push_back(col);
            }

            // Get indexes
            refreshedTable.indexes = getTableIndexes(tableName);

            // Get foreign keys
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

            // Find and update the table in our tables vector
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
