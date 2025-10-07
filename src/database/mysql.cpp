#include "database/mysql.hpp"
#include "database/db.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

MySQLDatabase::MySQLDatabase(const std::string& name, const std::string& host, int port,
                             const std::string& database, const std::string& username,
                             const std::string& password, bool showAllDatabases)
    : host(host), port(port), database(database), username(username), password(password),
      showAllDatabases(showAllDatabases) {
    this->name = name;
    std::cout << "DEBUG: Creating MySQLDatabase with database = '" << database
              << "', showAllDatabases = " << showAllDatabases << std::endl;
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
    loadingDatabases = false;
    switchingDatabase = false;

    tableDataLoader.cancelAllAndWait();

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

        // Verify the pool was created successfully
        auto* pool = getConnectionPoolForDatabase(database);
        if (!pool) {
            std::cerr << "ERROR: Connection pool was not created for database: " << database
                      << std::endl;
            connected = false;
            return {false, "Failed to create connection pool"};
        }
        std::cout << "DEBUG: Connection pool created successfully for database: " << database
                  << std::endl;

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
        if (!dbData.loadingTables.load()) {
            return result;
        }

        // Get table names using the session
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = "SHOW TABLES";
        {
            const auto sql = getSession();
            const soci::rowset tableRs = sql->prepare << tableNamesQuery;
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

            // Get table columns using the session
            const std::string columnsQuery = std::format("DESCRIBE `{}`", tableName);
            {
                const auto sql = getSession();
                const soci::rowset columnsRs = sql->prepare << columnsQuery;

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

            // Load indexes and foreign keys
            table.indexes = getTableIndexes(tableName);
            table.foreignKeys = getTableForeignKeys(tableName);
            buildForeignKeyLookup(table);

            result.push_back(table);
        }

        populateIncomingForeignKeys(result);
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
        if (!dbData.loadingViews.load()) {
            return result;
        }

        // Get view names using the session
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        {
            const auto sql = getSession();
            const soci::rowset viewRs = sql->prepare << viewNamesQuery;
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

            // Get view columns using the session (same as table columns for MySQL)
            const std::string columnsQuery = std::format("DESCRIBE `{}`", viewName);
            {
                const auto sql = getSession();
                const soci::rowset columnsRs = sql->prepare << columnsQuery;

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

void MySQLDatabase::refreshSequences() {
    getCurrentDatabaseData().sequences.clear();
    getCurrentDatabaseData().sequencesLoaded = true;
}

void MySQLDatabase::checkSequencesStatusAsync() {
    // No-op for MySQL
}

std::string MySQLDatabase::executeQuery(const std::string& query) {
    if (!connect().first) {
        return "Error: Not connected to database";
    }

    try {
        const auto sql = getSession();
        const soci::rowset rs = (sql->prepare << query);

        std::ostringstream result;
        bool first_row = true;

        for (auto& row : rs) {
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
        const auto sql = getSession();
        const soci::rowset rs = (sql->prepare << query);

        // Get column names if available
        const auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        int rowCount = 0;
        for (auto& row : rs) {
            if (rowCount >= 1000)
                break;

            std::vector<std::string> rowData;

            for (std::size_t i = 0; i != row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
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

std::vector<std::vector<std::string>>
MySQLDatabase::getTableData(const std::string& tableName, const int limit, const int offset) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::vector<std::string>> data;
    try {
        const auto sql = getSession();
        const std::string query = "SELECT * FROM `" + tableName + "` LIMIT " +
                                  std::to_string(limit) + " OFFSET " + std::to_string(offset);
        const soci::rowset rs = (sql->prepare << query);

        for (auto& row : rs) {
            std::vector<std::string> rowData;

            for (std::size_t i = 0; i != row.size(); ++i) {
                rowData.emplace_back(convertRowValue(row, i));
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
        const auto sql = getSession();
        const std::string query = "SHOW COLUMNS FROM `" + tableName + "`";
        const soci::rowset rs = (sql->prepare << query);

        for (auto& row : rs) {
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
        const auto session = getSession();
        int count = 0;
        const std::string query = std::format("SELECT COUNT(*) FROM `{}`", tableName);
        *session << query, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

void MySQLDatabase::startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                            const std::string& whereClause) {
    const bool started = tableDataLoader.start(tableName, [this, tableName, limit, offset,
                                                           whereClause](TableDataLoadState& state) {
        try {
            if (!state.loading.load()) {
                return;
            }

            std::string dataQuery = "SELECT * FROM `" + tableName + "`";
            if (!whereClause.empty()) {
                dataQuery += " WHERE " + whereClause;
            }
            dataQuery += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

            {
                const auto sql = getSession();
                const soci::rowset dataRs = sql->prepare << dataQuery;

                for (const auto& row : dataRs) {
                    if (!state.loading.load()) {
                        break;
                    }

                    std::vector<std::string> rowData;
                    rowData.reserve(row.size());
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        rowData.emplace_back(convertRowValue(row, i));
                    }
                    state.tableData.push_back(std::move(rowData));
                }
            }

            if (!state.loading.load()) {
                return;
            }

            const std::string columnQuery = "SHOW COLUMNS FROM `" + tableName + "`";
            {
                const auto sql = getSession();
                const soci::rowset columnRs = sql->prepare << columnQuery;

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

            std::string countQuery = "SELECT COUNT(*) FROM `" + tableName + "`";
            if (!whereClause.empty()) {
                countQuery = "SELECT COUNT(*) FROM `" + tableName + "` WHERE " + whereClause;
            }
            const auto sql = getSession();
            *sql << countQuery, soci::into(state.rowCount);

        } catch (const std::exception& e) {
            std::cerr << "Error in async table data load: " << e.what() << std::endl;
            state.tableData.clear();
            state.columnNames.clear();
            state.rowCount = 0;
            state.lastError = e.what();
        }
    });

    if (!started) {
        return;
    }
}

std::vector<std::string> MySQLDatabase::getTableNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> tableNames;
    try {
        const auto sql = getSession();
        const std::string query = "SHOW TABLES";
        const soci::rowset rs = (sql->prepare << query);

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
        const auto sql = getSession();
        const std::string query = std::format("DESCRIBE `{}`", tableName);
        const soci::rowset rs = (sql->prepare << query);

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

std::vector<Index> MySQLDatabase::getTableIndexes(const std::string& tableName) {
    std::vector<Index> indexes;

    if (!connect().first) {
        return indexes;
    }

    try {
        const auto sql = getSession();
        const std::string query = std::format("SHOW INDEX FROM `{}`", tableName);
        const soci::rowset rs = sql->prepare << query;

        std::unordered_map<std::string, Index> indexMap;

        for (const auto& row : rs) {
            std::string indexName = row.get<std::string>(2); // Key_name

            if (indexMap.find(indexName) == indexMap.end()) {
                Index idx;
                idx.name = indexName;
                idx.isUnique = row.get<int>(1) == 0; // Non_unique (0 means unique)
                idx.isPrimary = (indexName == "PRIMARY");
                idx.type = row.get<std::string>(10); // Index_type
                indexMap[indexName] = idx;
            }

            // Add column to the index
            std::string colName = row.get<std::string>(4); // Column_name
            indexMap[indexName].columns.push_back(colName);
        }

        // Convert map to vector
        for (auto& [name, idx] : indexMap) {
            indexes.push_back(idx);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting table indexes: " << e.what() << std::endl;
    }

    return indexes;
}

std::vector<ForeignKey> MySQLDatabase::getTableForeignKeys(const std::string& tableName) {
    std::vector<ForeignKey> foreignKeys;

    if (!connect().first) {
        return foreignKeys;
    }

    try {
        const auto sql = getSession();
        const std::string query =
            std::format("SELECT "
                        "    kcu.CONSTRAINT_NAME, "
                        "    kcu.COLUMN_NAME, "
                        "    kcu.REFERENCED_TABLE_NAME, "
                        "    kcu.REFERENCED_COLUMN_NAME, "
                        "    rc.UPDATE_RULE, "
                        "    rc.DELETE_RULE "
                        "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE kcu "
                        "JOIN INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS rc "
                        "    ON kcu.CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                        "    AND kcu.CONSTRAINT_SCHEMA = rc.CONSTRAINT_SCHEMA "
                        "WHERE kcu.TABLE_NAME = '{}' "
                        "AND kcu.TABLE_SCHEMA = DATABASE() "
                        "AND kcu.REFERENCED_TABLE_NAME IS NOT NULL",
                        tableName);

        const soci::rowset rs = sql->prepare << query;

        for (const auto& row : rs) {
            ForeignKey fk;
            fk.name = row.get<std::string>(0);
            fk.sourceColumn = row.get<std::string>(1);
            fk.targetTable = row.get<std::string>(2);
            fk.targetColumn = row.get<std::string>(3);
            fk.onUpdate = row.get<std::string>(4);
            fk.onDelete = row.get<std::string>(5);

            foreignKeys.push_back(fk);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "MySQL Error getting table foreign keys: " << e.what() << std::endl;
    }

    return foreignKeys;
}

std::vector<std::string> MySQLDatabase::getViewNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> viewNames;
    try {
        const auto sql = getSession();
        const std::string query = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        const soci::rowset rs = (sql->prepare << query);

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
        if (!loadingDatabases.load()) {
            return result;
        }

        // Check if we have a valid connection pool before trying to query
        if (!isConnected()) {
            std::cerr << "Cannot load databases: not connected" << std::endl;
            return result;
        }

        std::cout << "DEBUG: isConnected() = true, attempting to get session for database: "
                  << database << std::endl;

        const std::string sqlQuery = "SHOW DATABASES";

        std::cout << "Executing async query to get database names..." << std::endl;
        const auto sql = getSession();
        std::cout << "DEBUG: Session obtained successfully" << std::endl;
        const soci::rowset rs = sql->prepare << sqlQuery;

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
        setLastConnectionError(e.what());
        return {false, e.what()};
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        setLastConnectionError(e.what());
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
            } else {
                std::cout << "Async database switch failed to: " << targetDatabaseName << " - "
                          << error << std::endl;
                setLastConnectionError(error);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in async database switch: " << e.what() << std::endl;
            switchingDatabase = false;
            setLastConnectionError(e.what());
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

std::unique_ptr<soci::session> MySQLDatabase::getSession(const std::string& dbName) const {
    const std::string targetDb = dbName.empty() ? database : dbName;
    auto* pool = getConnectionPoolForDatabase(targetDb);
    if (!pool) {
        throw std::runtime_error("Connection pool not available for database: " + targetDb);
    }
    auto res = std::make_unique<soci::session>(*pool);
    if (!res->is_connected()) {
        res->reconnect();
    }
    return res;
}
