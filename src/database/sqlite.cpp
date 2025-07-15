#include "database/sqlite.hpp"
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
    } catch (const soci::soci_error &e) {
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

const std::string &SQLiteDatabase::getName() const {
    return name;
}

const std::string &SQLiteDatabase::getConnectionString() const {
    return path;
}

const std::string &SQLiteDatabase::getPath() const {
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
    std::vector<std::string> tableNames = getTableNames();
    std::cout << "Found " << tableNames.size() << " tables" << std::endl;

    for (const auto &tableName : tableNames) {
        std::cout << "Adding table: " << tableName << std::endl;
        Table table;
        table.name = tableName;
        table.columns = getTableColumns(tableName);
        tables.push_back(table);
    }
    std::cout << "Finished refreshing tables. Total tables: " << tables.size() << std::endl;
    tablesLoaded = true;
}

const std::vector<Table> &SQLiteDatabase::getTables() const {
    return tables;
}

std::vector<Table> &SQLiteDatabase::getTables() {
    return tables;
}

bool SQLiteDatabase::areTablesLoaded() const {
    return tablesLoaded;
}

void SQLiteDatabase::setTablesLoaded(bool loaded) {
    tablesLoaded = loaded;
}

std::string SQLiteDatabase::executeQuery(const std::string &query) {
    if (!isConnected()) {
        return "Error: Failed to connect to database";
    }

    try {
        std::stringstream result;
        soci::rowset<soci::row> rs = session->prepare << query;

        // Get column names if available
        auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row &firstRow = *it;
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
        for (const auto &row : rs) {
            if (rowCount >= 1000)
                break;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    result << "NULL";
                } else {
                    try {
                        result << row.get<std::string>(i);
                    } catch (const std::bad_cast &) {
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
    } catch (const soci::soci_error &e) {
        return "Error: " + std::string(e.what());
    }
}

std::vector<std::vector<std::string>>
SQLiteDatabase::getTableData(const std::string &tableName, const int limit, const int offset) {
    std::vector<std::vector<std::string>> data;
    if (!isConnected()) {
        return data;
    }

    try {
        const std::string sql = "SELECT * FROM " + tableName + " LIMIT " + std::to_string(limit) +
                                " OFFSET " + std::to_string(offset);

        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            std::vector<std::string> rowData;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                } else {
                    try {
                        rowData.emplace_back(row.get<std::string>(i));
                    } catch (const std::bad_cast &) {
                        rowData.emplace_back("[BINARY DATA]");
                    }
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }
    return data;
}

std::vector<std::string> SQLiteDatabase::getColumnNames(const std::string &tableName) {
    std::vector<std::string> columnNames;
    if (!isConnected()) {
        return columnNames;
    }

    try {
        const std::string sql = "PRAGMA table_info(" + tableName + ");";
        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            columnNames.emplace_back(row.get<std::string>(1));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }
    return columnNames;
}

int SQLiteDatabase::getRowCount(const std::string &tableName) {
    if (!isConnected()) {
        return 0;
    }

    try {
        const std::string sql = "SELECT COUNT(*) FROM " + tableName;
        int count = 0;
        *session << sql, soci::into(count);
        return count;
    } catch (const soci::soci_error &e) {
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

const std::string &SQLiteDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void SQLiteDatabase::setLastConnectionError(const std::string &error) {
    lastConnectionError = error;
}

void *SQLiteDatabase::getConnection() const {
    return session.get();
}

std::vector<std::string> SQLiteDatabase::getTableNames() {
    std::vector<std::string> tableNames;
    const char *sql = "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;";

    std::cout << "Executing query to get table names..." << std::endl;
    try {
        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            std::string tableName = row.get<std::string>(0);
            std::cout << "Found table: " << tableName << std::endl;
            tableNames.emplace_back(tableName);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute SQL statement: " << e.what() << std::endl;
    }
    std::cout << "Query completed. Found " << tableNames.size() << " tables." << std::endl;
    return tableNames;
}

std::vector<Column> SQLiteDatabase::getTableColumns(const std::string &tableName) {
    std::vector<Column> columns;
    std::string sql = "PRAGMA table_info(" + tableName + ");";

    try {
        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            Column col;
            col.name = row.get<std::string>(1);
            col.type = row.get<std::string>(2);
            auto notNullStr = row.get<std::string>(3);
            col.isNotNull = (notNullStr == "1" || notNullStr == "true");
            auto pkStr = row.get<std::string>(5);
            col.isPrimaryKey = (pkStr == "1" || pkStr == "true");
            columns.push_back(col);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting table columns: " << e.what() << std::endl;
    }
    return columns;
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
    std::vector<std::string> viewNames = getViewNames();
    std::cout << "Found " << viewNames.size() << " views" << std::endl;

    for (const auto &viewName : viewNames) {
        std::cout << "Adding view: " << viewName << std::endl;
        Table view;
        view.name = viewName;
        view.columns = getViewColumns(viewName);
        views.push_back(view);
    }
    std::cout << "Finished refreshing views. Total views: " << views.size() << std::endl;
    viewsLoaded = true;
}

const std::vector<Table> &SQLiteDatabase::getViews() const {
    return views;
}

std::vector<Table> &SQLiteDatabase::getViews() {
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

const std::vector<std::string> &SQLiteDatabase::getSequences() const {
    return sequences;
}

std::vector<std::string> &SQLiteDatabase::getSequences() {
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
    const char *sql = "SELECT name FROM sqlite_master WHERE type = 'view' ORDER BY name;";

    std::cout << "Executing query to get view names..." << std::endl;
    try {
        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            auto viewName = row.get<std::string>(0);
            std::cout << "Found view: " << viewName << std::endl;
            viewNames.emplace_back(viewName);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute SQL statement: " << e.what() << std::endl;
    }
    std::cout << "Query completed. Found " << viewNames.size() << " views." << std::endl;
    return viewNames;
}

std::vector<Column> SQLiteDatabase::getViewColumns(const std::string &viewName) {
    std::vector<Column> columns;
    std::string sql = "PRAGMA table_info(" + viewName + ");";

    try {
        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            Column col;
            col.name = row.get<std::string>(1);
            col.type = row.get<std::string>(2);
            auto notNullStr = row.get<std::string>(3);
            col.isNotNull = (notNullStr == "1" || notNullStr == "true");
            col.isPrimaryKey = false; // Views don't have primary keys
            columns.push_back(col);
        }
    } catch (const soci::soci_error &e) {
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

void SQLiteDatabase::startAsyncConnection() {
    // For SQLite, just call the synchronous connect method
    // since file-based connections are typically fast
    connect();
}

void SQLiteDatabase::checkAsyncConnectionStatus() {
    // No-op for SQLite since connection is synchronous
}
