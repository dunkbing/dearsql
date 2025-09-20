#include "database/sqlite.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <typeinfo>
#include <utility>

SQLiteDatabase::SQLiteDatabase(std::string name, std::string path)
    : name(std::move(name)), path(std::move(path)) {}

SQLiteDatabase::~SQLiteDatabase() {
    SQLiteDatabase::disconnect();
}

std::pair<bool, std::string> SQLiteDatabase::connect() {
    if (connected && session) {
        return {true, ""};
    }

    attemptedConnection = true;

    try {
        session = std::make_unique<soci::session>(soci::sqlite3, path);
        std::cout << "Successfully connected to database: " << path << std::endl;
        connected = true;
        lastConnectionError.clear();
        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::string error = e.what();
        std::cerr << "Can't open database: " << error << std::endl;
        lastConnectionError = error;
        return {false, error};
    }
}

void SQLiteDatabase::disconnect() {
    if (session) {
        session.reset();
    }
    connected = false;
}

bool SQLiteDatabase::isConnected() const {
    return connected;
}

const std::string& SQLiteDatabase::getName() const {
    return name;
}

const std::string& SQLiteDatabase::getConnectionString() const {
    return path;
}

const std::string& SQLiteDatabase::getPath() const {
    return path;
}

DatabaseType SQLiteDatabase::getType() const {
    return DatabaseType::SQLITE;
}

void SQLiteDatabase::refreshTables() {
    std::cout << "Refreshing tables for database: " << name << std::endl;
    if (!isConnected()) {
        std::cout << "Failed to connect to database" << std::endl;
        tablesLoaded = true;
        return;
    }

    tables.clear();
    const std::vector<std::string> tableNames = getTableNames();
    std::cout << "Found " << tableNames.size() << " tables" << std::endl;

    for (const auto& tableName : tableNames) {
        std::cout << "Adding table: " << tableName << std::endl;
        Table table;
        table.name = tableName;
        table.fullName = name + "." + tableName; // SQLite: connection.table
        table.columns = getTableColumns(tableName);
        table.indexes = getTableIndexes(tableName);
        table.foreignKeys = getTableForeignKeys(tableName);

        // Build foreign key lookup map
        for (const auto& fk : table.foreignKeys) {
            table.foreignKeysByColumn[fk.sourceColumn] = fk;
        }

        tables.push_back(table);
    }

    // Build incoming foreign key references
    // After all tables are loaded, find which tables reference each table
    for (auto& targetTable : tables) {
        for (const auto& sourceTable : tables) {
            for (const auto& fk : sourceTable.foreignKeys) {
                if (fk.targetTable == targetTable.name) {
                    // Create an incoming foreign key entry
                    ForeignKey incomingFK;
                    incomingFK.name = fk.name;
                    incomingFK.sourceColumn = fk.sourceColumn;
                    incomingFK.targetTable = sourceTable.name; // The table that has the FK
                    incomingFK.targetColumn = fk.targetColumn;
                    incomingFK.onDelete = fk.onDelete;
                    incomingFK.onUpdate = fk.onUpdate;
                    targetTable.incomingForeignKeys.push_back(incomingFK);
                }
            }
        }
    }

    std::cout << "Finished refreshing tables. Total tables: " << tables.size() << std::endl;
    tablesLoaded = true;
}

const std::vector<Table>& SQLiteDatabase::getTables() const {
    return tables;
}

std::vector<Table>& SQLiteDatabase::getTables() {
    return tables;
}

bool SQLiteDatabase::areTablesLoaded() const {
    return tablesLoaded;
}

void SQLiteDatabase::setTablesLoaded(bool loaded) {
    tablesLoaded = loaded;
}

std::string SQLiteDatabase::executeQuery(const std::string& query) {
    if (!isConnected()) {
        return "Error: Failed to connect to database";
    }

    try {
        std::stringstream result;
        const soci::rowset rs = session->prepare << query;

        // Get column names if available
        auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                result << firstRow.get_properties(i).get_name();
                if (i < firstRow.size() - 1)
                    result << " | ";
            }
            result << "\n";

            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                result << "----------";
                if (i < firstRow.size() - 1)
                    result << "-+-";
            }
            result << "\n";
        }

        int rowCount = 0;
        for (const auto& row : rs) {
            if (rowCount >= 1000)
                break;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    result << "NULL";
                } else {
                    try {
                        result << row.get<std::string>(i);
                    } catch (const std::bad_cast&) {
                        result << "[BINARY DATA]";
                    }
                }
                if (i < row.size() - 1)
                    result << " | ";
            }
            result << "\n";
            rowCount++;
        }

        if (rowCount == 0) {
            result << "Query executed successfully.";
        } else if (rowCount == 1000) {
            result << "\n... (showing first 1000 rows)";
        }

        return result.str();
    } catch (const soci::soci_error& e) {
        return "Error: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
SQLiteDatabase::executeQueryStructured(const std::string& query) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!isConnected()) {
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
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                } else {
                    try {
                        rowData.push_back(row.get<std::string>(i));
                    } catch (const std::bad_cast&) {
                        rowData.emplace_back("[BINARY DATA]");
                    }
                }
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

std::vector<std::vector<std::string>>
SQLiteDatabase::getTableData(const std::string& tableName, const int limit, const int offset) {
    std::vector<std::vector<std::string>> data;
    if (!isConnected()) {
        return data;
    }

    try {
        const std::string sql =
            std::format("SELECT * FROM {} LIMIT {} OFFSET {}", tableName, limit, offset);

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
                case soci::db_wstring:
                    // Convert wide string to UTF-8 string
                    {
                        auto ws = row.get<std::wstring>(i);
                        std::string utf8_str(ws.begin(), ws.end());
                        rowData.emplace_back(utf8_str);
                    }
                    break;
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

std::vector<std::string> SQLiteDatabase::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;
    if (!isConnected()) {
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

int SQLiteDatabase::getRowCount(const std::string& tableName) {
    if (!isConnected()) {
        return 0;
    }

    try {
        const std::string sql = "SELECT COUNT(*) FROM " + tableName;
        int count = 0;
        *session << sql, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

bool SQLiteDatabase::isExpanded() const {
    return expanded;
}

void SQLiteDatabase::setExpanded(bool exp) {
    expanded = exp;
}

bool SQLiteDatabase::hasAttemptedConnection() const {
    return attemptedConnection;
}

void SQLiteDatabase::setAttemptedConnection(bool attempted) {
    attemptedConnection = attempted;
}

const std::string& SQLiteDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void SQLiteDatabase::setLastConnectionError(const std::string& error) {
    lastConnectionError = error;
}

void* SQLiteDatabase::getConnection() const {
    return session.get();
}

std::vector<std::string> SQLiteDatabase::getTableNames() {
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

std::vector<Column> SQLiteDatabase::getTableColumns(const std::string& tableName) {
    std::vector<Column> columns;
    const std::string sql = std::format("PRAGMA table_info('{}');", tableName);

    try {
        const soci::rowset rs = session->prepare << sql;

        for (const auto& row : rs) {
            Column col;
            col.name = convertRowValue(row, 1);
            col.type = convertRowValue(row, 2);

            // Handle notnull column (can be int or string)
            std::string notNullStr = convertRowValue(row, 3);
            col.isNotNull = (notNullStr == "1" || notNullStr == "true");

            // Handle primary key column (can be int or string)
            std::string pkStr = convertRowValue(row, 5);
            col.isPrimaryKey = (pkStr == "1" || pkStr == "true");

            columns.push_back(col);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table columns: " << e.what() << std::endl;
    }
    return columns;
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

// View management methods
void SQLiteDatabase::refreshViews() {
    std::cout << "Refreshing views for database: " << name << std::endl;
    if (!isConnected()) {
        std::cout << "Failed to connect to database" << std::endl;
        viewsLoaded = true;
        return;
    }

    views.clear();
    const std::vector<std::string> viewNames = getViewNames();
    std::cout << "Found " << viewNames.size() << " views" << std::endl;

    for (const auto& viewName : viewNames) {
        std::cout << "Adding view: " << viewName << std::endl;
        Table view;
        view.name = viewName;
        view.fullName = name + "." + viewName; // SQLite: connection.view
        view.columns = getViewColumns(viewName);
        views.push_back(view);
    }
    std::cout << "Finished refreshing views. Total views: " << views.size() << std::endl;
    viewsLoaded = true;
}

const std::vector<Table>& SQLiteDatabase::getViews() const {
    return views;
}

std::vector<Table>& SQLiteDatabase::getViews() {
    return views;
}

bool SQLiteDatabase::areViewsLoaded() const {
    return viewsLoaded;
}

void SQLiteDatabase::setViewsLoaded(bool loaded) {
    viewsLoaded = loaded;
}

// Sequence management methods (not applicable for SQLite)
void SQLiteDatabase::refreshSequences() {
    sequences.clear();
    sequencesLoaded = true; // No sequences in SQLite
}

const std::vector<std::string>& SQLiteDatabase::getSequences() const {
    return sequences;
}

std::vector<std::string>& SQLiteDatabase::getSequences() {
    return sequences;
}

bool SQLiteDatabase::areSequencesLoaded() const {
    return sequencesLoaded;
}

void SQLiteDatabase::setSequencesLoaded(bool loaded) {
    sequencesLoaded = loaded;
}

std::vector<std::string> SQLiteDatabase::getViewNames() {
    std::vector<std::string> viewNames;
    const auto sql = "SELECT name FROM sqlite_master WHERE type = 'view' ORDER BY name;";

    std::cout << "Executing query to get view names..." << std::endl;
    try {
        const soci::rowset rs = session->prepare << sql;

        for (const auto& row : rs) {
            auto viewName = row.get<std::string>(0);
            std::cout << "Found view: " << viewName << std::endl;
            viewNames.emplace_back(viewName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute SQL statement: " << e.what() << std::endl;
    }
    std::cout << "Query completed. Found " << viewNames.size() << " views." << std::endl;
    return viewNames;
}

std::vector<Column> SQLiteDatabase::getViewColumns(const std::string& viewName) {
    std::vector<Column> columns;
    const std::string sql = "PRAGMA table_info(" + viewName + ");";

    try {
        const soci::rowset rs = session->prepare << sql;

        for (const auto& row : rs) {
            Column col;
            col.name = convertRowValue(row, 1);
            col.type = convertRowValue(row, 2);

            // Handle notnull column (can be int or string)
            std::string notNullStr = convertRowValue(row, 3);
            col.isNotNull = (notNullStr == "1" || notNullStr == "true");

            col.isPrimaryKey = false; // Views don't have primary keys
            columns.push_back(col);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting view columns: " << e.what() << std::endl;
    }
    return columns;
}

std::vector<std::string> SQLiteDatabase::getSequenceNames() {
    return {}; // SQLite doesn't have sequences
}

bool SQLiteDatabase::isConnecting() const {
    return false; // SQLite connections are synchronous and fast
}

void SQLiteDatabase::startConnectionAsync() {
    // For SQLite, just call the synchronous connect method
    // since file-based connections are typically fast
    connect();
}

void SQLiteDatabase::checkConnectionStatusAsync() {
    // No-op for SQLite since connection is synchronous
}

// Async table data loading methods
void SQLiteDatabase::startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                             const std::string& whereClause) {
    if (loadingTableData) {
        return; // Already loading
    }

    loadingTableData = true;
    hasTableDataReady = false;
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;

    // Start async operation that loads everything
    tableDataFuture =
        std::async(std::launch::async, [this, tableName, limit, offset, whereClause]() {
            try {
                if (!whereClause.empty()) {
                    // For filtered queries, use executeQueryStructured
                    const std::string dataQuery = "SELECT * FROM \"" + tableName + "\" WHERE " +
                                                  whereClause + " LIMIT " + std::to_string(limit) +
                                                  " OFFSET " + std::to_string(offset);
                    auto [columns, data] = executeQueryStructured(dataQuery);
                    tableDataResult = data;
                    columnNamesResult = columns;

                    // Get filtered count
                    const std::string countQuery =
                        "SELECT COUNT(*) FROM \"" + tableName + "\" WHERE " + whereClause;
                    auto [countCols, countData] = executeQueryStructured(countQuery);
                    if (!countData.empty() && !countData[0].empty()) {
                        rowCountResult = std::stoi(countData[0][0]);
                    } else {
                        rowCountResult = 0;
                    }
                } else {
                    // No filter - use existing methods
                    tableDataResult = getTableData(tableName, limit, offset);
                    columnNamesResult = getColumnNames(tableName);
                    rowCountResult = getRowCount(tableName);
                }
            } catch (const std::exception& e) {
                std::cerr << "Error in async table data load: " << e.what() << std::endl;
                // Clear results on error
                tableDataResult.clear();
                columnNamesResult.clear();
                rowCountResult = 0;
            }
        });
}

bool SQLiteDatabase::isLoadingTableData() const {
    return loadingTableData;
}

void SQLiteDatabase::checkTableDataStatusAsync() {
    if (!loadingTableData) {
        return;
    }

    if (tableDataFuture.valid() &&
        tableDataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tableDataFuture.get(); // This will throw if there was an exception
            hasTableDataReady = true;
            loadingTableData = false;
        } catch (const std::exception& e) {
            std::cerr << "Error loading table data: " << e.what() << std::endl;
            loadingTableData = false;
            hasTableDataReady = false;
            // Clear results on error
            tableDataResult.clear();
            columnNamesResult.clear();
            rowCountResult = 0;
        }
    }
}

bool SQLiteDatabase::hasTableDataResult() const {
    return hasTableDataReady;
}

std::vector<std::vector<std::string>> SQLiteDatabase::getTableDataResult() {
    if (hasTableDataReady) {
        return tableDataResult;
    }
    return {};
}

std::vector<std::string> SQLiteDatabase::getColumnNamesResult() {
    if (hasTableDataReady) {
        return columnNamesResult;
    }
    return {};
}

int SQLiteDatabase::getRowCountResult() {
    if (hasTableDataReady) {
        return rowCountResult;
    }
    return 0;
}

void SQLiteDatabase::clearTableDataResult() {
    hasTableDataReady = false;
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;
}
