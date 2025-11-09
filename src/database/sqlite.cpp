#include "database/sqlite.hpp"
#include "database/sqlite/sqlite_database_node.hpp"
#include <iostream>
#include <memory>
#include <sstream>
#include <typeinfo>
#include <utility>

SQLiteDatabase::SQLiteDatabase(std::string name_, std::string path) : path(std::move(path)) {
    name = std::move(name_);

    // Create the database node
    databaseNode = std::make_shared<SQLiteDatabaseNode>();
    databaseNode->parentDb = this;
    databaseNode->name = name;
}

SQLiteDatabase::~SQLiteDatabase() {
    SQLiteDatabase::disconnect();
}

std::pair<bool, std::string> SQLiteDatabase::connect() {
    if (connected && session) {
        return {true, ""};
    }

    try {
        session = std::make_unique<soci::session>(soci::sqlite3, path);
        std::cout << "Successfully connected to database: " << path << std::endl;
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
        buildForeignKeyLookup(table);

        tables.push_back(table);
    }

    populateIncomingForeignKeys(tables);

    std::cout << "Finished refreshing tables. Total tables: " << tables.size() << std::endl;
    tablesLoaded = true;
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

std::vector<std::vector<std::string>> SQLiteDatabase::getTableData(const std::string& tableName,
                                                                   int limit, int offset) {
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

// Sequence management methods (not applicable for SQLite)
void SQLiteDatabase::refreshSequences() {
    sequences.clear();
    sequencesLoaded = true; // No sequences in SQLite
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

// Async table data loading (delegates to TableDataLoader in base class)
void SQLiteDatabase::startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                             const std::string& whereClause) {
    tableDataLoader.start(
        tableName, [this, tableName, limit, offset, whereClause](TableDataLoadState& state) {
            try {
                if (!whereClause.empty()) {
                    // For filtered queries, use executeQueryStructured
                    const std::string dataQuery = "SELECT * FROM \"" + tableName + "\" WHERE " +
                                                  whereClause + " LIMIT " + std::to_string(limit) +
                                                  " OFFSET " + std::to_string(offset);
                    auto [columns, data] = executeQueryStructured(dataQuery);
                    state.tableData = std::move(data);
                    state.columnNames = std::move(columns);

                    // Get filtered count
                    const std::string countQuery =
                        "SELECT COUNT(*) FROM \"" + tableName + "\" WHERE " + whereClause;
                    auto [countCols, countData] = executeQueryStructured(countQuery);
                    if (!countData.empty() && !countData[0].empty()) {
                        state.rowCount = std::stoi(countData[0][0]);
                    } else {
                        state.rowCount = 0;
                    }
                } else {
                    // No filter - use existing methods
                    state.tableData = getTableData(tableName, limit, offset);
                    state.columnNames = getColumnNames(tableName);
                    state.rowCount = getRowCount(tableName);
                }
                state.ready = true;
            } catch (const std::exception& e) {
                std::cerr << "Error in async table data load: " << e.what() << std::endl;
                state.lastError = e.what();
                state.tableData.clear();
                state.columnNames.clear();
                state.rowCount = 0;
            }
        });
}

std::shared_ptr<SQLiteDatabaseNode> SQLiteDatabase::getDatabaseNode() const {
    return databaseNode;
}

soci::session* SQLiteDatabase::getSession() const {
    if (!isConnected() || !session) {
        return nullptr;
    }
    return session.get();
}
