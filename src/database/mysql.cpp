#include "database/mysql.hpp"
#include <iostream>
#include <sstream>

MySQLDatabase::MySQLDatabase(const std::string &name, const std::string &host, int port,
                             const std::string &database, const std::string &username,
                             const std::string &password, bool showAllDatabases)
    : name(name), host(host), port(port), database(database), username(username),
      password(password), showAllDatabases(showAllDatabases) {
    // SOCI MySQL connection string format: "host=hostname port=port dbname=database user=username
    // password=password" Use TCP/IP connection to avoid Unix socket issues
    connectionString = "host=" + host + " port=" + std::to_string(port) + " dbname=" + database;

    if (!username.empty()) {
        connectionString += " user=" + username;
    }

    if (!password.empty()) {
        connectionString += " password=" + password;
    }
}

MySQLDatabase::~MySQLDatabase() {
    // Stop all async operations before cleaning up
    connecting = false;
    loadingTableData = false;

    // Stop all per-database async operations
    for (auto &[dbName, dbData] : databaseDataCache) {
        dbData.loadingTables = false;
        dbData.loadingViews = false;

        // Wait for all futures to complete
        if (dbData.tablesFuture.valid()) {
            dbData.tablesFuture.wait();
        }
        if (dbData.viewsFuture.valid()) {
            dbData.viewsFuture.wait();
        }
    }

    if (connectionFuture.valid()) {
        connectionFuture.wait();
    }
    if (tableDataFuture.valid()) {
        tableDataFuture.wait();
    }

    disconnect();
}

// Helper methods for per-database data access
MySQLDatabase::DatabaseData &MySQLDatabase::getCurrentDatabaseData() {
    return databaseDataCache[database];
}

const MySQLDatabase::DatabaseData &MySQLDatabase::getCurrentDatabaseData() const {
    static const DatabaseData emptyData;
    auto it = databaseDataCache.find(database);
    return (it != databaseDataCache.end()) ? it->second : emptyData;
}

MySQLDatabase::DatabaseData &MySQLDatabase::getDatabaseData(const std::string &dbName) {
    return databaseDataCache[dbName];
}

const MySQLDatabase::DatabaseData &MySQLDatabase::getDatabaseData(const std::string &dbName) const {
    static const DatabaseData emptyData;
    auto it = databaseDataCache.find(dbName);
    return (it != databaseDataCache.end()) ? it->second : emptyData;
}

std::pair<bool, std::string> MySQLDatabase::connect() {
    // Check if we already have a connection to the current database
    auto *session = getSessionForDatabase(database);
    if (connected && session) {
        return {true, ""};
    }

    try {
        std::lock_guard lock(sessionMutex);
        sessionPool[database] = std::make_unique<soci::session>(soci::mysql, connectionString);
        connected = true;
        return {true, ""};
    } catch (const soci::soci_error &e) {
        connected = false;
        std::lock_guard lock(sessionMutex);
        sessionPool.erase(database);
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    } catch (const std::exception &e) {
        connected = false;
        std::lock_guard lock(sessionMutex);
        sessionPool.erase(database);
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MySQLDatabase::disconnect() {
    std::lock_guard lock(sessionMutex);
    sessionPool.clear();
    connected = false;
    // Don't clear per-database cache on disconnect
}

bool MySQLDatabase::isConnected() const {
    return connected && getSessionForDatabase(database) != nullptr;
}

bool MySQLDatabase::isConnecting() const {
    return connecting.load();
}

void MySQLDatabase::startConnectionAsync() {
    if (connecting.load()) {
        return;
    }

    connecting.store(true);
    connectionFuture = std::async(std::launch::async, [this]() {
        auto result = connect();
        connecting.store(false);
        return result;
    });
}

void MySQLDatabase::checkConnectionStatusAsync() {
    if (connectionFuture.valid() &&
        connectionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = connectionFuture.get();
        setAttemptedConnection(true);
        if (!result.first) {
            setLastConnectionError(result.second);
        }
    }
}

const std::string &MySQLDatabase::getName() const {
    return name;
}

const std::string &MySQLDatabase::getConnectionString() const {
    return connectionString;
}

const std::string &MySQLDatabase::getPath() const {
    static std::string empty;
    return empty;
}

void *MySQLDatabase::getConnection() const {
    return getSessionForDatabase(database);
}

DatabaseType MySQLDatabase::getType() const {
    return DatabaseType::MYSQL;
}

const std::string &MySQLDatabase::getDatabaseName() const {
    return database;
}

void MySQLDatabase::refreshTables() {
    if (isLoadingTables()) {
        return;
    }
    startRefreshTableAsync();
}

void MySQLDatabase::startRefreshTableAsync() {
    auto &dbData = getCurrentDatabaseData();
    if (dbData.loadingTables.load()) {
        return;
    }

    dbData.loadingTables.store(true);
    dbData.tablesFuture =
        std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getTablesWithColumnsAsync() {
    auto &dbData = getCurrentDatabaseData();
    if (!connect().first || !dbData.loadingTables.load()) {
        return {};
    }

    std::vector<Table> result;
    std::vector<std::string> tableNames = getTableNames();

    if (!dbData.loadingTables.load()) {
        return result;
    }

    for (const auto &tableName : tableNames) {
        if (!dbData.loadingTables.load()) {
            break; // Stop processing if we should no longer be loading
        }

        Table table;
        table.name = tableName;
        table.columns = getTableColumns(tableName);
        result.push_back(table);
    }

    return result;
}

void MySQLDatabase::checkTablesStatusAsync() {
    auto &dbData = getCurrentDatabaseData();
    if (dbData.tablesFuture.valid() &&
        dbData.tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        dbData.tables = dbData.tablesFuture.get();
        dbData.tablesLoaded = true;
        dbData.loadingTables.store(false);
    }
}

const std::vector<Table> &MySQLDatabase::getTables() const {
    return getCurrentDatabaseData().tables;
}

std::vector<Table> &MySQLDatabase::getTables() {
    return getCurrentDatabaseData().tables;
}

bool MySQLDatabase::areTablesLoaded() const {
    return getCurrentDatabaseData().tablesLoaded;
}

void MySQLDatabase::setTablesLoaded(bool loaded) {
    getCurrentDatabaseData().tablesLoaded = loaded;
}

bool MySQLDatabase::isLoadingTables() const {
    return getCurrentDatabaseData().loadingTables.load();
}

void MySQLDatabase::refreshViews() {
    if (isLoadingViews()) {
        return;
    }
    startRefreshViewAsync();
}

void MySQLDatabase::startRefreshViewAsync() {
    auto &dbData = getCurrentDatabaseData();
    if (dbData.loadingViews.load()) {
        return;
    }

    dbData.loadingViews.store(true);
    dbData.viewsFuture =
        std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getViewsWithColumnsAsync() {
    auto &dbData = getCurrentDatabaseData();
    if (!connect().first || !dbData.loadingViews.load()) {
        return {};
    }

    std::vector<Table> result;
    std::vector<std::string> viewNames = getViewNames();

    if (!dbData.loadingViews.load()) {
        return result;
    }

    for (const auto &viewName : viewNames) {
        if (!dbData.loadingViews.load()) {
            break; // Stop processing if we should no longer be loading
        }

        Table view;
        view.name = viewName;
        view.columns = getViewColumns(viewName);
        result.push_back(view);
    }

    return result;
}

void MySQLDatabase::checkViewsStatusAsync() {
    auto &dbData = getCurrentDatabaseData();
    if (dbData.viewsFuture.valid() &&
        dbData.viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        dbData.views = dbData.viewsFuture.get();
        dbData.viewsLoaded = true;
        dbData.loadingViews.store(false);
    }
}

const std::vector<Table> &MySQLDatabase::getViews() const {
    return getCurrentDatabaseData().views;
}

std::vector<Table> &MySQLDatabase::getViews() {
    return getCurrentDatabaseData().views;
}

bool MySQLDatabase::areViewsLoaded() const {
    return getCurrentDatabaseData().viewsLoaded;
}

void MySQLDatabase::setViewsLoaded(bool loaded) {
    getCurrentDatabaseData().viewsLoaded = loaded;
}

bool MySQLDatabase::isLoadingViews() const {
    return getCurrentDatabaseData().loadingViews.load();
}

void MySQLDatabase::refreshSequences() {
    getCurrentDatabaseData().sequences.clear();
    getCurrentDatabaseData().sequencesLoaded = true;
}

const std::vector<std::string> &MySQLDatabase::getSequences() const {
    return getCurrentDatabaseData().sequences;
}

std::vector<std::string> &MySQLDatabase::getSequences() {
    return getCurrentDatabaseData().sequences;
}

bool MySQLDatabase::areSequencesLoaded() const {
    return getCurrentDatabaseData().sequencesLoaded;
}

void MySQLDatabase::setSequencesLoaded(bool loaded) {
    getCurrentDatabaseData().sequencesLoaded = loaded;
}

bool MySQLDatabase::isLoadingSequences() const {
    return false;
}

void MySQLDatabase::checkSequencesStatusAsync() {
    // No-op for MySQL
}

std::string MySQLDatabase::executeQuery(const std::string &query) {
    if (!connect().first) {
        return "Error: Not connected to database";
    }

    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return "Error: Database session is not available";
        }
        std::lock_guard lock(sessionMutex);

        const soci::rowset rs = (session->prepare << query);

        std::ostringstream result;
        bool first_row = true;

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;

            if (first_row) {
                for (std::size_t i = 0; i != row.size(); ++i) {
                    if (i > 0)
                        result << "\t";
                    result << row.get_properties(i).get_name();
                }
                result << "\n";
                first_row = false;
            }

            for (std::size_t i = 0; i != row.size(); ++i) {
                if (i > 0)
                    result << "\t";

                if (row.get_indicator(i) == soci::i_null) {
                    result << "NULL";
                } else {
                    result << row.get<std::string>(i, "");
                }
            }
            result << "\n";
        }

        return result.str();
    } catch (const soci::soci_error &e) {
        return "MySQL Error: " + std::string(e.what());
    } catch (const std::exception &e) {
        return "Error: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
MySQLDatabase::executeQueryStructured(const std::string &query) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!connect().first) {
        return {columnNames, data};
    }

    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return {columnNames, data};
        }
        std::lock_guard lock(sessionMutex);

        const soci::rowset rs = (session->prepare << query);

        // Get column names if available
        const auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row &firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        int rowCount = 0;
        for (auto rowIt = rs.begin(); rowIt != rs.end(); ++rowIt) {
            if (rowCount >= 1000)
                break;

            const soci::row &row = *rowIt;
            std::vector<std::string> rowData;

            for (std::size_t i = 0; i != row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.push_back("NULL");
                } else {
                    rowData.push_back(row.get<std::string>(i, ""));
                }
            }
            data.push_back(rowData);
            rowCount++;
        }

        return {columnNames, data};
    } catch (const soci::soci_error &e) {
        std::cout << "[soci] MySQL Error: " + std::string(e.what());
        return {columnNames, data};
    } catch (const std::exception &e) {
        std::cout << "MySQL Error: " + std::string(e.what());
        return {columnNames, data};
    }
}

std::vector<std::vector<std::string>> MySQLDatabase::getTableData(const std::string &tableName,
                                                                  int limit, int offset) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::vector<std::string>> data;
    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return data;
        }
        std::lock_guard lock(sessionMutex);

        std::string query = "SELECT * FROM `" + tableName + "` LIMIT " + std::to_string(limit) +
                            " OFFSET " + std::to_string(offset);
        soci::rowset rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            std::vector<std::string> rowData;

            for (std::size_t i = 0; i != row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                    continue;
                }
                switch (soci::column_properties cp = row.get_properties(i); cp.get_db_type()) {
                case soci::db_string:
                    rowData.emplace_back(row.get<std::string>(i));
                    break;
                case soci::db_wstring:
                    // convert to UTF-8 string
                    {
                        auto ws = row.get<std::wstring>(i);
                        std::string utf8_str(ws.begin(), ws.end());
                        rowData.emplace_back(utf8_str);
                    }
                    break;
                case soci::db_int8:
                    rowData.emplace_back(std::to_string(row.get<int8_t>(i)));
                    break;
                case soci::db_uint8:
                    rowData.emplace_back(std::to_string(row.get<uint8_t>(i)));
                    break;
                case soci::db_int16:
                    rowData.emplace_back(std::to_string(row.get<int16_t>(i)));
                    break;
                case soci::db_uint16:
                    rowData.emplace_back(std::to_string(row.get<uint16_t>(i)));
                    break;
                case soci::db_int32:
                    rowData.emplace_back(std::to_string(row.get<int32_t>(i)));
                    break;
                case soci::db_uint32:
                    rowData.emplace_back(std::to_string(row.get<uint32_t>(i)));
                    break;
                case soci::db_int64:
                    rowData.emplace_back(std::to_string(row.get<int64_t>(i)));
                    break;
                case soci::db_uint64:
                    rowData.emplace_back(std::to_string(row.get<uint64_t>(i)));
                    break;
                case soci::db_double:
                    rowData.emplace_back(std::to_string(row.get<double>(i)));
                    break;
                case soci::db_date: {
                    auto date = row.get<std::tm>(i);
                    char buffer[32];
                    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &date);
                    rowData.emplace_back(buffer);
                } break;
                case soci::db_blob:
                    rowData.emplace_back("[BINARY DATA]");
                    break;
                case soci::db_xml:
                    try {
                        rowData.emplace_back(row.get<std::string>(i));
                    } catch (const std::bad_cast &) {
                        rowData.emplace_back("[XML DATA]");
                    }
                    break;
                default: {
                    // Log the unknown type for debugging
                    std::cout << "Unknown MySQL data type: " << static_cast<int>(cp.get_db_type())
                              << " for column: " << cp.get_name() << std::endl;

                    try {
                        rowData.emplace_back(row.get<std::string>(i));
                    } catch (const std::bad_cast &) {
                        rowData.emplace_back("[UNKNOWN DATA TYPE: " +
                                             std::to_string(static_cast<int>(cp.get_db_type())) +
                                             "]");
                    }
                } break;
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting table data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> MySQLDatabase::getColumnNames(const std::string &tableName) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> columns;
    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return columns;
        }
        std::lock_guard lock(sessionMutex);

        const std::string query = "SHOW COLUMNS FROM `" + tableName + "`";
        const soci::rowset rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            columns.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "[soci] MySQL Error getting column names: " << e.what() << std::endl;
    }

    return columns;
}

int MySQLDatabase::getRowCount(const std::string &tableName) {
    if (!connect().first) {
        return 0;
    }

    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return 0;
        }
        std::lock_guard lock(sessionMutex);

        int count = 0;
        const std::string query = std::format("SELECT COUNT(*) FROM `{}`", tableName);
        *session << query, soci::into(count);
        return count;
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

void MySQLDatabase::startTableDataLoadAsync(const std::string &tableName, int limit, int offset) {
    if (loadingTableData.load()) {
        return;
    }

    loadingTableData.store(true);
    hasTableDataReady.store(false);

    tableDataFuture = std::async(std::launch::async, [this, tableName, limit, offset]() {
        if (!loadingTableData.load()) {
            return;
        }

        tableDataResult = getTableData(tableName, limit, offset);

        if (!loadingTableData.load()) {
            return;
        }

        columnNamesResult = getColumnNames(tableName);

        if (!loadingTableData.load()) {
            return;
        }

        rowCountResult = getRowCount(tableName);

        if (loadingTableData.load()) {
            hasTableDataReady.store(true);
        }

        loadingTableData.store(false);
    });
}

bool MySQLDatabase::isLoadingTableData() const {
    return loadingTableData.load();
}

void MySQLDatabase::checkTableDataStatusAsync() {
    if (!loadingTableData.load()) {
        return;
    }

    if (tableDataFuture.valid() &&
        tableDataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tableDataFuture.get(); // This will throw if there was an exception
            // The async operation handles setting hasTableDataReady and loadingTableData flags
        } catch (const std::exception &e) {
            std::cerr << "Error loading table data: " << e.what() << std::endl;
            loadingTableData.store(false);
            hasTableDataReady.store(false);
            // Clear results on error
            tableDataResult.clear();
            columnNamesResult.clear();
            rowCountResult = 0;
        }
    }
}

bool MySQLDatabase::hasTableDataResult() const {
    return hasTableDataReady.load();
}

std::vector<std::vector<std::string>> MySQLDatabase::getTableDataResult() {
    return tableDataResult;
}

std::vector<std::string> MySQLDatabase::getColumnNamesResult() {
    return columnNamesResult;
}

int MySQLDatabase::getRowCountResult() {
    return rowCountResult;
}

void MySQLDatabase::clearTableDataResult() {
    hasTableDataReady.store(false);
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;
}

bool MySQLDatabase::isExpanded() const {
    return expanded;
}

void MySQLDatabase::setExpanded(bool expanded) {
    this->expanded = expanded;
}

bool MySQLDatabase::hasAttemptedConnection() const {
    return attemptedConnection;
}

void MySQLDatabase::setAttemptedConnection(bool attempted) {
    attemptedConnection = attempted;
}

const std::string &MySQLDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void MySQLDatabase::setLastConnectionError(const std::string &error) {
    lastConnectionError = error;
}

std::vector<std::string> MySQLDatabase::getTableNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> tableNames;
    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return tableNames;
        }
        std::lock_guard lock(sessionMutex);

        const std::string query = "SHOW TABLES";
        const soci::rowset rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            tableNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting table names: " << e.what() << std::endl;
    }

    return tableNames;
}

std::vector<Column> MySQLDatabase::getTableColumns(const std::string &tableName) {
    if (!connect().first) {
        return {};
    }

    std::vector<Column> columns;
    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return columns;
        }
        std::lock_guard lock(sessionMutex);

        const std::string query = std::format("DESCRIBE `{}`", tableName);
        const soci::rowset rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            Column col;
            col.name = row.get<std::string>(0);                  // Field
            col.type = row.get<std::string>(1);                  // Type
            col.isNotNull = row.get<std::string>(2) == "NO";     // Null
            col.isPrimaryKey = row.get<std::string>(3) == "PRI"; // Key
            columns.push_back(col);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting table columns: " << e.what() << std::endl;
    }

    return columns;
}

std::vector<std::string> MySQLDatabase::getViewNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> viewNames;
    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return viewNames;
        }
        std::lock_guard lock(sessionMutex);

        const std::string query = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        const soci::rowset rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            viewNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting view names: " << e.what() << std::endl;
    }

    return viewNames;
}

std::vector<Column> MySQLDatabase::getViewColumns(const std::string &viewName) {
    return getTableColumns(viewName);
}

std::vector<std::string> MySQLDatabase::getSequenceNames() {
    return {};
}

std::vector<std::string> MySQLDatabase::getDatabaseNames() {
    if (databasesLoaded) {
        return availableDatabases;
    }

    availableDatabases.clear();

    try {
        auto *session = getSessionForDatabase(database);
        if (!session) {
            return availableDatabases;
        }
        std::lock_guard lock(sessionMutex);

        const std::string sql = "SHOW DATABASES";

        std::cout << "Executing query to get database names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            auto dbName = row.get<std::string>(0);
            // Filter out system databases
            if (dbName != "information_schema" && dbName != "performance_schema" &&
                dbName != "mysql" && dbName != "sys") {
                std::cout << "Found database: " << dbName << std::endl;
                availableDatabases.push_back(dbName);
            }
        }

        databasesLoaded = true;
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute database query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << availableDatabases.size() << " databases."
              << std::endl;
    return availableDatabases;
}

std::pair<bool, std::string> MySQLDatabase::switchToDatabase(const std::string &targetDatabase) {
    if (database == targetDatabase && connected) {
        return {true, ""}; // Already connected to the target database
    }

    // Update database name and connection string
    database = targetDatabase;
    connectionString = buildConnectionString(targetDatabase);

    // Check if we already have a connection to the target database
    auto *session = getSessionForDatabase(targetDatabase);
    if (session) {
        connected = true;
        std::cout << "Reusing existing connection to database: " << targetDatabase << std::endl;
        return {true, ""};
    }

    // Create new connection to the target database
    try {
        std::lock_guard lock(sessionMutex);
        sessionPool[targetDatabase] =
            std::make_unique<soci::session>(soci::mysql, connectionString);
        connected = true;
        std::cout << "Created new connection to database: " << targetDatabase << std::endl;
        return {true, ""};
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    } catch (const std::exception &e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    }
}

bool MySQLDatabase::isDatabaseExpanded(const std::string &dbName) const {
    return expandedDatabases.find(dbName) != expandedDatabases.end();
}

void MySQLDatabase::setDatabaseExpanded(const std::string &dbName, bool expanded) {
    if (expanded) {
        expandedDatabases.insert(dbName);
    } else {
        expandedDatabases.erase(dbName);
    }
}

soci::session *MySQLDatabase::getSessionForDatabase(const std::string &dbName) const {
    std::lock_guard lock(sessionMutex);
    auto it = sessionPool.find(dbName);
    return (it != sessionPool.end()) ? it->second.get() : nullptr;
}

std::string MySQLDatabase::buildConnectionString(const std::string &dbName) const {
    std::string connStr = "host=" + host + " port=" + std::to_string(port) + " dbname=" + dbName;

    if (!username.empty()) {
        connStr += " user=" + username;
    }

    if (!password.empty()) {
        connStr += " password=" + password;
    }

    return connStr;
}
