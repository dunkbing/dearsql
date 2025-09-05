#include "database/mysql.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>

MySQLDatabase::MySQLDatabase(const std::string& name, const std::string& host, int port,
                             const std::string& database, const std::string& username,
                             const std::string& password, bool showAllDatabases)
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
    loadingDatabases = false;
    switchingDatabase = false;

    // Stop all table data loading operations
    for (auto& [tableName, state] : tableDataStates) {
        state.loading.store(false);
        if (state.future.valid()) {
            state.future.wait();
        }
    }

    // Stop all per-database async operations
    for (auto& [dbName, dbData] : databaseDataCache) {
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
    if (databasesFuture.valid()) {
        databasesFuture.wait();
    }
    if (databaseSwitchFuture.valid()) {
        databaseSwitchFuture.wait();
    }

    disconnect();
}

// Helper methods for per-database data access
MySQLDatabase::DatabaseData& MySQLDatabase::getCurrentDatabaseData() {
    return databaseDataCache[database];
}

const MySQLDatabase::DatabaseData& MySQLDatabase::getCurrentDatabaseData() const {
    static const DatabaseData emptyData;
    auto it = databaseDataCache.find(database);
    return (it != databaseDataCache.end()) ? it->second : emptyData;
}

MySQLDatabase::DatabaseData& MySQLDatabase::getDatabaseData(const std::string& dbName) {
    return databaseDataCache[dbName];
}

const MySQLDatabase::DatabaseData& MySQLDatabase::getDatabaseData(const std::string& dbName) const {
    static const DatabaseData emptyData;
    auto it = databaseDataCache.find(dbName);
    return (it != databaseDataCache.end()) ? it->second : emptyData;
}

std::pair<bool, std::string> MySQLDatabase::connect() {
    // Check if we already have a connection pool to the current database
    auto* pool = getConnectionPoolForDatabase(database);
    if (connected && pool) {
        return {true, ""};
    }

    try {
        initializeConnectionPool(database, connectionString);
        connected = true;

        // Start loading databases immediately if showAllDatabases is enabled
        if (showAllDatabases && !databasesLoaded && !loadingDatabases.load()) {
            std::cout << "Starting async database loading after connection..." << std::endl;
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const soci::soci_error& e) {
        connected = false;
        std::lock_guard lock(sessionMutex);
        connectionPools.erase(database);
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    } catch (const std::exception& e) {
        connected = false;
        std::lock_guard lock(sessionMutex);
        connectionPools.erase(database);
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MySQLDatabase::disconnect() {
    std::lock_guard lock(sessionMutex);
    connectionPools.clear();
    connected = false;
    // Don't clear per-database cache on disconnect
}

bool MySQLDatabase::isConnected() const {
    return connected && getConnectionPoolForDatabase(database) != nullptr;
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
        if (result.first) {
            // Start loading databases if showAllDatabases is enabled and not already loading
            if (showAllDatabases && !databasesLoaded && !loadingDatabases.load()) {
                std::cout << "Starting async database loading after async connection..."
                          << std::endl;
                refreshDatabaseNames();
            }
        } else {
            setLastConnectionError(result.second);
        }
    }
}

const std::string& MySQLDatabase::getName() const {
    return name;
}

const std::string& MySQLDatabase::getConnectionString() const {
    return connectionString;
}

const std::string& MySQLDatabase::getPath() const {
    static std::string empty;
    return empty;
}

void* MySQLDatabase::getConnection() const {
    return getConnectionPoolForDatabase(database);
}

DatabaseType MySQLDatabase::getType() const {
    return DatabaseType::MYSQL;
}

const std::string& MySQLDatabase::getDatabaseName() const {
    return database;
}

void MySQLDatabase::refreshTables() {
    if (isLoadingTables()) {
        return;
    }
    startRefreshTableAsync();
}

void MySQLDatabase::startRefreshTableAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (dbData.loadingTables.load()) {
        return;
    }

    dbData.loadingTables.store(true);
    dbData.tablesFuture =
        std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getTablesWithColumnsAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (!dbData.loadingTables.load()) {
        return {};
    }

    std::vector<Table> result;

    try {
        // Use connection pool instead of creating a new session
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            std::cerr << "Connection pool not available for table loading" << std::endl;
            return result;
        }

        if (!dbData.loadingTables.load()) {
            return result;
        }

        // Get table names using the connection pool
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = "SHOW TABLES";
        {
            soci::session sql(*pool);
            const soci::rowset tableRs = sql.prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!dbData.loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        if (!dbData.loadingTables.load()) {
            return result;
        }

        for (const auto& tableName : tableNames) {
            if (!dbData.loadingTables.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table table;
            table.name = tableName;
            table.fullName =
                name + "." + database + "." + tableName; // MySQL: connection.database.table

            // Get table columns using the connection pool
            const std::string columnsQuery = std::format("DESCRIBE `{}`", tableName);
            {
                soci::session sql(*pool);
                const soci::rowset columnsRs = sql.prepare << columnsQuery;

                for (const auto& colRow : columnsRs) {
                    if (!dbData.loadingTables.load()) {
                        break;
                    }

                    Column col;
                    col.name = colRow.get<std::string>(0);                  // Field
                    col.type = colRow.get<std::string>(1);                  // Type
                    col.isNotNull = colRow.get<std::string>(2) == "NO";     // Null
                    col.isPrimaryKey = colRow.get<std::string>(3) == "PRI"; // Key
                    table.columns.push_back(col);
                }
            }

            result.push_back(table);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting tables with columns: " << e.what() << std::endl;
    }

    return result;
}

void MySQLDatabase::checkTablesStatusAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (dbData.tablesFuture.valid() &&
        dbData.tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        dbData.tables = dbData.tablesFuture.get();
        dbData.tablesLoaded = true;
        dbData.loadingTables.store(false);
    }
}

const std::vector<Table>& MySQLDatabase::getTables() const {
    return getCurrentDatabaseData().tables;
}

std::vector<Table>& MySQLDatabase::getTables() {
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
    auto& dbData = getCurrentDatabaseData();
    if (dbData.loadingViews.load()) {
        return;
    }

    dbData.loadingViews.store(true);
    dbData.viewsFuture =
        std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getViewsWithColumnsAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (!dbData.loadingViews.load()) {
        return {};
    }

    std::vector<Table> result;

    try {
        // Use connection pool instead of creating a new session
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            std::cerr << "Connection pool not available for view loading" << std::endl;
            return result;
        }

        if (!dbData.loadingViews.load()) {
            return result;
        }

        // Get view names using the connection pool
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        {
            soci::session sql(*pool);
            const soci::rowset viewRs = sql.prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!dbData.loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        if (!dbData.loadingViews.load()) {
            return result;
        }

        for (const auto& viewName : viewNames) {
            if (!dbData.loadingViews.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table view;
            view.name = viewName;
            view.fullName =
                name + "." + database + "." + viewName; // MySQL: connection.database.view

            // Get view columns using the connection pool (same as table columns for MySQL)
            const std::string columnsQuery = std::format("DESCRIBE `{}`", viewName);
            {
                soci::session sql(*pool);
                const soci::rowset columnsRs = sql.prepare << columnsQuery;

                for (const auto& colRow : columnsRs) {
                    if (!dbData.loadingViews.load()) {
                        break;
                    }

                    Column col;
                    col.name = colRow.get<std::string>(0);                  // Field
                    col.type = colRow.get<std::string>(1);                  // Type
                    col.isNotNull = colRow.get<std::string>(2) == "NO";     // Null
                    col.isPrimaryKey = colRow.get<std::string>(3) == "PRI"; // Key
                    view.columns.push_back(col);
                }
            }

            result.push_back(view);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting views with columns: " << e.what() << std::endl;
    }

    return result;
}

void MySQLDatabase::checkViewsStatusAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (dbData.viewsFuture.valid() &&
        dbData.viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        dbData.views = dbData.viewsFuture.get();
        dbData.viewsLoaded = true;
        dbData.loadingViews.store(false);
    }
}

const std::vector<Table>& MySQLDatabase::getViews() const {
    return getCurrentDatabaseData().views;
}

std::vector<Table>& MySQLDatabase::getViews() {
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

const std::vector<std::string>& MySQLDatabase::getSequences() const {
    return getCurrentDatabaseData().sequences;
}

std::vector<std::string>& MySQLDatabase::getSequences() {
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

std::string MySQLDatabase::executeQuery(const std::string& query) {
    if (!connect().first) {
        return "Error: Not connected to database";
    }

    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return "Error: Database connection pool is not available";
        }

        soci::session sql(*pool);
        const soci::rowset rs = (sql.prepare << query);

        std::ostringstream result;
        bool first_row = true;

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row& row = *it;

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
    } catch (const soci::soci_error& e) {
        return "MySQL Error: " + std::string(e.what());
    } catch (const std::exception& e) {
        return "Error: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
MySQLDatabase::executeQueryStructured(const std::string& query) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!connect().first) {
        return {columnNames, data};
    }

    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return {columnNames, data};
        }

        soci::session sql(*pool);
        const soci::rowset rs = (sql.prepare << query);

        // Get column names if available
        const auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        int rowCount = 0;
        for (auto rowIt = rs.begin(); rowIt != rs.end(); ++rowIt) {
            if (rowCount >= 1000)
                break;

            const soci::row& row = *rowIt;
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
    } catch (const soci::soci_error& e) {
        std::cout << "[soci] MySQL Error: " + std::string(e.what());
        return {columnNames, data};
    } catch (const std::exception& e) {
        std::cout << "MySQL Error: " + std::string(e.what());
        return {columnNames, data};
    }
}

std::vector<std::vector<std::string>> MySQLDatabase::getTableData(const std::string& tableName,
                                                                  int limit, int offset) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::vector<std::string>> data;
    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return data;
        }

        soci::session sql(*pool);
        std::string query = "SELECT * FROM `" + tableName + "` LIMIT " + std::to_string(limit) +
                            " OFFSET " + std::to_string(offset);
        soci::rowset rs = (sql.prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row& row = *it;
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
                    } catch (const std::bad_cast&) {
                        rowData.emplace_back("[XML DATA]");
                    }
                    break;
                default: {
                    // Log the unknown type for debugging
                    std::cout << "Unknown MySQL data type: " << static_cast<int>(cp.get_db_type())
                              << " for column: " << cp.get_name() << std::endl;

                    try {
                        rowData.emplace_back(row.get<std::string>(i));
                    } catch (const std::bad_cast&) {
                        rowData.emplace_back("[UNKNOWN DATA TYPE: " +
                                             std::to_string(static_cast<int>(cp.get_db_type())) +
                                             "]");
                    }
                } break;
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting table data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> MySQLDatabase::getColumnNames(const std::string& tableName) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> columns;
    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return columns;
        }

        soci::session sql(*pool);
        const std::string query = "SHOW COLUMNS FROM `" + tableName + "`";
        const soci::rowset rs = (sql.prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row& row = *it;
            columns.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "[soci] MySQL Error getting column names: " << e.what() << std::endl;
    }

    return columns;
}

int MySQLDatabase::getRowCount(const std::string& tableName) {
    if (!connect().first) {
        return 0;
    }

    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return 0;
        }

        soci::session sql(*pool);
        int count = 0;
        const std::string query = std::format("SELECT COUNT(*) FROM `{}`", tableName);
        sql << query, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

void MySQLDatabase::startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                            const std::string& whereClause) {
    auto& state = tableDataStates[tableName];

    if (state.loading.load()) {
        return;
    }

    state.loading.store(true);
    state.ready.store(false);
    state.tableData.clear();
    state.columnNames.clear();
    state.rowCount = 0;

    state.future = std::async(std::launch::async, [this, tableName, limit, offset, whereClause]() {
        try {
            // Use connection pool instead of creating a new session
            auto* pool = getConnectionPoolForDatabase(database);
            if (!pool) {
                std::cerr << "Connection pool not available for table data loading" << std::endl;
                auto& state = tableDataStates[tableName];
                state.loading.store(false);
                return;
            }

            // Get reference to the state for this table
            auto& state = tableDataStates[tableName];

            if (!state.loading.load()) {
                return;
            }

            // Load table data
            std::string dataQuery = "SELECT * FROM `" + tableName + "`";
            if (!whereClause.empty()) {
                dataQuery += " WHERE " + whereClause;
            }
            dataQuery += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

            {
                soci::session sql(*pool);
                const soci::rowset dataRs = sql.prepare << dataQuery;

                for (const auto& row : dataRs) {
                    if (!state.loading.load()) {
                        break; // Stop if we should no longer be loading
                    }

                    std::vector<std::string> rowData;
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        if (row.get_indicator(i) == soci::i_null) {
                            rowData.emplace_back("NULL");
                        } else {
                            try {
                                rowData.emplace_back(row.get<std::string>(i));
                            } catch (const std::bad_cast&) {
                                rowData.emplace_back("[BINARY DATA]");
                            }
                        }
                    }
                    state.tableData.push_back(rowData);
                }
            }

            if (!state.loading.load()) {
                return;
            }

            // Load column names
            const std::string columnQuery = "SHOW COLUMNS FROM `" + tableName + "`";
            {
                soci::session sql(*pool);
                const soci::rowset columnRs = sql.prepare << columnQuery;

                for (const auto& row : columnRs) {
                    if (!state.loading.load()) {
                        break;
                    }
                    state.columnNames.push_back(row.get<std::string>(0));
                }
            }

            if (!state.loading.load()) {
                return;
            }

            // Load row count
            std::string countQuery = "SELECT COUNT(*) FROM `" + tableName + "`";
            if (!whereClause.empty()) {
                countQuery = "SELECT COUNT(*) FROM `" + tableName + "` WHERE " + whereClause;
            }
            {
                soci::session sql(*pool);
                sql << countQuery, soci::into(state.rowCount);
            }

            if (state.loading.load()) {
                state.ready.store(true);
            }

        } catch (const std::exception& e) {
            std::cerr << "Error in async table data load: " << e.what() << std::endl;
            // Clear results on error
            auto& state = tableDataStates[tableName];
            state.tableData.clear();
            state.columnNames.clear();
            state.rowCount = 0;
        }

        auto& state = tableDataStates[tableName];
        state.loading.store(false);
    });
}

bool MySQLDatabase::isLoadingTableData(const std::string& tableName) const {
    auto it = tableDataStates.find(tableName);
    return it != tableDataStates.end() && it->second.loading.load();
}

bool MySQLDatabase::isLoadingTableData() const {
    // Legacy method - return true if any table is loading
    for (const auto& [name, state] : tableDataStates) {
        if (state.loading.load()) {
            return true;
        }
    }
    return false;
}

void MySQLDatabase::checkTableDataStatusAsync(const std::string& tableName) {
    auto it = tableDataStates.find(tableName);
    if (it == tableDataStates.end() || !it->second.loading.load()) {
        return;
    }

    auto& state = it->second;
    if (state.future.valid() &&
        state.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            state.future.get(); // This will throw if there was an exception
            // The async operation handles setting ready and loading flags
        } catch (const std::exception& e) {
            std::cerr << "Error loading table data for " << tableName << ": " << e.what()
                      << std::endl;
            state.loading.store(false);
            state.ready.store(false);
            // Clear results on error
            state.tableData.clear();
            state.columnNames.clear();
            state.rowCount = 0;
        }
    }
}

void MySQLDatabase::checkTableDataStatusAsync() {
    // Legacy method - check all tables
    for (auto& [tableName, state] : tableDataStates) {
        if (state.loading.load()) {
            checkTableDataStatusAsync(tableName);
        }
    }
}

bool MySQLDatabase::hasTableDataResult(const std::string& tableName) const {
    auto it = tableDataStates.find(tableName);
    return it != tableDataStates.end() && it->second.ready.load();
}

bool MySQLDatabase::hasTableDataResult() const {
    // Legacy method - return true if any table has results
    for (const auto& [name, state] : tableDataStates) {
        if (state.ready.load()) {
            return true;
        }
    }
    return false;
}

std::vector<std::vector<std::string>>
MySQLDatabase::getTableDataResult(const std::string& tableName) {
    auto it = tableDataStates.find(tableName);
    if (it != tableDataStates.end() && it->second.ready.load()) {
        return it->second.tableData;
    }
    return {};
}

std::vector<std::vector<std::string>> MySQLDatabase::getTableDataResult() {
    // Legacy method - return first available result
    for (const auto& [name, state] : tableDataStates) {
        if (state.ready.load()) {
            return state.tableData;
        }
    }
    return {};
}

std::vector<std::string> MySQLDatabase::getColumnNamesResult(const std::string& tableName) {
    auto it = tableDataStates.find(tableName);
    if (it != tableDataStates.end() && it->second.ready.load()) {
        return it->second.columnNames;
    }
    return {};
}

std::vector<std::string> MySQLDatabase::getColumnNamesResult() {
    // Legacy method - return first available result
    for (const auto& [name, state] : tableDataStates) {
        if (state.ready.load()) {
            return state.columnNames;
        }
    }
    return {};
}

int MySQLDatabase::getRowCountResult(const std::string& tableName) {
    auto it = tableDataStates.find(tableName);
    if (it != tableDataStates.end() && it->second.ready.load()) {
        return it->second.rowCount;
    }
    return 0;
}

int MySQLDatabase::getRowCountResult() {
    // Legacy method - return first available result
    for (const auto& [name, state] : tableDataStates) {
        if (state.ready.load()) {
            return state.rowCount;
        }
    }
    return 0;
}

void MySQLDatabase::clearTableDataResult(const std::string& tableName) {
    auto it = tableDataStates.find(tableName);
    if (it != tableDataStates.end()) {
        auto& state = it->second;
        state.ready.store(false);
        state.tableData.clear();
        state.columnNames.clear();
        state.rowCount = 0;
    }
}

void MySQLDatabase::clearTableDataResult() {
    // Legacy method - clear all results
    for (auto& [name, state] : tableDataStates) {
        state.ready.store(false);
        state.tableData.clear();
        state.columnNames.clear();
        state.rowCount = 0;
    }
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

const std::string& MySQLDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void MySQLDatabase::setLastConnectionError(const std::string& error) {
    lastConnectionError = error;
}

std::vector<std::string> MySQLDatabase::getTableNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> tableNames;
    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return tableNames;
        }

        soci::session sql(*pool);
        const std::string query = "SHOW TABLES";
        const soci::rowset rs = (sql.prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row& row = *it;
            tableNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting table names: " << e.what() << std::endl;
    }

    return tableNames;
}

std::vector<Column> MySQLDatabase::getTableColumns(const std::string& tableName) {
    if (!connect().first) {
        return {};
    }

    std::vector<Column> columns;
    try {
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return columns;
        }

        soci::session sql(*pool);
        const std::string query = std::format("DESCRIBE `{}`", tableName);
        const soci::rowset rs = (sql.prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row& row = *it;
            Column col;
            col.name = row.get<std::string>(0);                  // Field
            col.type = row.get<std::string>(1);                  // Type
            col.isNotNull = row.get<std::string>(2) == "NO";     // Null
            col.isPrimaryKey = row.get<std::string>(3) == "PRI"; // Key
            columns.push_back(col);
        }
    } catch (const soci::soci_error& e) {
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
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            return viewNames;
        }

        soci::session sql(*pool);
        const std::string query = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        const soci::rowset rs = (sql.prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row& row = *it;
            viewNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting view names: " << e.what() << std::endl;
    }

    return viewNames;
}

std::vector<Column> MySQLDatabase::getViewColumns(const std::string& viewName) {
    return getTableColumns(viewName);
}

std::vector<std::string> MySQLDatabase::getSequenceNames() {
    return {};
}

std::vector<std::string> MySQLDatabase::getDatabaseNames() {
    if (databasesLoaded) {
        return availableDatabases;
    }

    // Start async loading if not already loading and we're connected
    if (!loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }

    return availableDatabases; // Return current state (may be empty if still loading)
}

void MySQLDatabase::refreshDatabaseNames() {
    if (loadingDatabases.load()) {
        return; // Already loading
    }

    startRefreshDatabasesAsync();
}

bool MySQLDatabase::isLoadingDatabases() const {
    return loadingDatabases.load();
}

void MySQLDatabase::checkDatabasesStatusAsync() {
    if (databasesFuture.valid() &&
        databasesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            availableDatabases = databasesFuture.get();
            std::cout << "Async database loading completed. Found " << availableDatabases.size()
                      << " databases." << std::endl;
            databasesLoaded = true;
            loadingDatabases = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async database loading: " << e.what() << std::endl;
            databasesLoaded = true; // Mark as loaded to prevent retry loops
            loadingDatabases = false;
        }
    }
}

void MySQLDatabase::startRefreshDatabasesAsync() {
    // Clear previous results
    availableDatabases.clear();
    databasesLoaded = false;
    loadingDatabases = true;

    // Start async loading with std::async
    databasesFuture = std::async(std::launch::async, [this]() { return getDatabaseNamesAsync(); });
}

std::vector<std::string> MySQLDatabase::getDatabaseNamesAsync() const {
    std::vector<std::string> result;

    // Check if we're still supposed to be loading
    if (!loadingDatabases.load()) {
        return result;
    }

    try {
        // Use connection pool instead of creating a new session
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            std::cerr << "Connection pool not available for database query" << std::endl;
            return result;
        }

        if (!loadingDatabases.load()) {
            return result;
        }

        const std::string sqlQuery = "SHOW DATABASES";

        std::cout << "Executing async query to get database names..." << std::endl;
        soci::session sql(*pool);
        const soci::rowset rs = sql.prepare << sqlQuery;

        for (const auto& row : rs) {
            if (!loadingDatabases.load()) {
                break; // Stop processing if we should no longer be loading
            }

            auto dbName = row.get<std::string>(0);
            // Filter out system databases
            if (dbName != "information_schema" && dbName != "performance_schema" &&
                dbName != "mysql" && dbName != "sys") {
                std::cout << "Found database: " << dbName << std::endl;
                result.push_back(dbName);
            }
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute async database query: " << e.what() << std::endl;
    }

    std::cout << "Async query completed. Found " << result.size() << " databases." << std::endl;
    return result;
}

std::pair<bool, std::string> MySQLDatabase::switchToDatabase(const std::string& targetDatabase) {
    if (database == targetDatabase && connected) {
        return {true, ""}; // Already connected to the target database
    }

    // Update database name and connection string
    database = targetDatabase;
    connectionString = buildConnectionString(targetDatabase);

    // Check if we already have a connection pool to the target database
    auto* pool = getConnectionPoolForDatabase(targetDatabase);
    if (pool) {
        connected = true;
        std::cout << "Reusing existing connection pool to database: " << targetDatabase
                  << std::endl;
        return {true, ""};
    }

    // Create new connection pool to the target database
    try {
        initializeConnectionPool(targetDatabase, connectionString);
        connected = true;
        std::cout << "Created new connection pool to database: " << targetDatabase << std::endl;
        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    }
}

void MySQLDatabase::switchToDatabaseAsync(const std::string& targetDatabase) {
    if (switchingDatabase.load()) {
        return; // Already switching
    }

    targetDatabaseName = targetDatabase;
    switchingDatabase = true;

    // Start async database switching
    databaseSwitchFuture = std::async(
        std::launch::async, [this, targetDatabase]() { return switchToDatabase(targetDatabase); });
}

bool MySQLDatabase::isSwitchingDatabase() const {
    return switchingDatabase.load();
}

void MySQLDatabase::checkDatabaseSwitchStatusAsync() {
    if (databaseSwitchFuture.valid() &&
        databaseSwitchFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto [success, error] = databaseSwitchFuture.get();
            switchingDatabase = false;

            if (success) {
                std::cout << "Async database switch completed successfully to: "
                          << targetDatabaseName << std::endl;
                // The database and connection state are already updated by switchToDatabase

                // Auto-start loading tables if not already loaded
                if (!areTablesLoaded() && !isLoadingTables()) {
                    std::cout << "Auto-starting table loading after database switch" << std::endl;
                    refreshTables();
                }
            } else {
                std::cout << "Async database switch failed to: " << targetDatabaseName << " - "
                          << error << std::endl;
                lastConnectionError = error;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in async database switch: " << e.what() << std::endl;
            switchingDatabase = false;
            lastConnectionError = e.what();
        }
    }
}

bool MySQLDatabase::isDatabaseExpanded(const std::string& dbName) const {
    return expandedDatabases.find(dbName) != expandedDatabases.end();
}

void MySQLDatabase::setDatabaseExpanded(const std::string& dbName, bool expanded) {
    if (expanded) {
        expandedDatabases.insert(dbName);
    } else {
        expandedDatabases.erase(dbName);
    }
}

soci::connection_pool*
MySQLDatabase::getConnectionPoolForDatabase(const std::string& dbName) const {
    std::lock_guard lock(sessionMutex);
    auto it = connectionPools.find(dbName);
    return (it != connectionPools.end()) ? it->second.get() : nullptr;
}

void MySQLDatabase::initializeConnectionPool(const std::string& dbName,
                                             const std::string& connStr) {
    std::lock_guard lock(sessionMutex);

    // Don't recreate if pool already exists
    if (connectionPools.find(dbName) != connectionPools.end()) {
        return;
    }

    const size_t poolSize = 10;
    auto pool = std::make_unique<soci::connection_pool>(poolSize);

    // Initialize connections in parallel for faster startup
    std::vector<std::future<void>> connectionFutures;
    connectionFutures.reserve(poolSize);

    for (size_t i = 0; i != poolSize; ++i) {
        connectionFutures.emplace_back(std::async(std::launch::async, [&pool, i, connStr]() {
            soci::session& sql = pool->at(i);
            sql.open(soci::mysql, connStr);
        }));
    }

    // Wait for all connections to complete
    for (auto& future : connectionFutures) {
        future.wait();
    }

    connectionPools[dbName] = std::move(pool);
}

std::string MySQLDatabase::buildConnectionString(const std::string& dbName) const {
    std::string connStr = "host=" + host + " port=" + std::to_string(port) + " dbname=" + dbName;

    if (!username.empty()) {
        connStr += " user=" + username;
    }

    if (!password.empty()) {
        connStr += " password=" + password;
    }

    return connStr;
}
