#include "database/mysql.hpp"
#include "database/db.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <vector>

MySQLDatabase::MySQLDatabase(const DatabaseConnectionInfo& connInfo)
    : connectionInfo(connInfo), database(connInfo.database) {
    this->name = connInfo.name;
    Logger::debug(
        std::format("DEBUG: Creating MySQLDatabase with database = '{}', showAllDatabases = {}",
                    database, connInfo.showAllDatabases));
    connectionString = buildConnectionString(database);
}

MySQLDatabase::~MySQLDatabase() {
    // Stop all async operations before cleaning up
    loadingDatabases = false;

    // Stop all per-database async operations
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->loadingTables = false;
            dbDataPtr->loadingViews = false;

            // Wait for all futures to complete
            if (dbDataPtr->tablesFuture.valid()) {
                dbDataPtr->tablesFuture.wait();
            }
            if (dbDataPtr->viewsFuture.valid()) {
                dbDataPtr->viewsFuture.wait();
            }
        }
    }

    if (databasesFuture.valid()) {
        databasesFuture.wait();
    }

    disconnect();
}

void MySQLDatabase::setConnectionInfo(const DatabaseConnectionInfo& info) {
    connectionInfo = info;
    database = info.database;
    name = info.name;
    connectionString = buildConnectionString(database);
}

// Helper methods for per-database data access
MySQLDatabaseNode* MySQLDatabase::getCurrentDatabaseData() {
    const auto it = databaseDataCache.find(database);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<MySQLDatabaseNode>();
        newData->name = database;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[database] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

const MySQLDatabaseNode* MySQLDatabase::getCurrentDatabaseData() const {
    const auto it = databaseDataCache.find(database);
    return (it != databaseDataCache.end()) ? it->second.get() : nullptr;
}

MySQLDatabaseNode* MySQLDatabase::getDatabaseData(const std::string& dbName) {
    const auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        // Create new MySQLDatabaseNode with the name set
        auto newData = std::make_unique<MySQLDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

const MySQLDatabaseNode* MySQLDatabase::getDatabaseData(const std::string& dbName) const {
    const auto it = databaseDataCache.find(dbName);
    return (it != databaseDataCache.end()) ? it->second.get() : nullptr;
}

std::pair<bool, std::string> MySQLDatabase::connect(bool forceRefresh) {
    // Check if we already have a connection pool to the current database
    const auto* pool = getConnectionPoolForDatabase(database);
    if (connected && pool) {
        return {true, ""};
    }

    try {
        initializeConnectionPool(database, connectionString);
        connected = true;

        // Verify the pool was created successfully
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
        // Clear connection pool from DatabaseData
        auto* dbData = getDatabaseData(database);
        if (dbData) {
            dbData->connectionPool.reset();
        }
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    } catch (const std::exception& e) {
        connected = false;
        std::lock_guard lock(sessionMutex);
        // Clear connection pool from DatabaseData
        auto* dbData = getDatabaseData(database);
        if (dbData) {
            dbData->connectionPool.reset();
        }
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MySQLDatabase::disconnect() {
    std::lock_guard lock(sessionMutex);
    // Clear all connection pools
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->connectionPool.reset();
        }
    }
    connected = false;
}

const std::string& MySQLDatabase::getName() const {
    return name;
}

const std::string& MySQLDatabase::getConnectionString() const {
    return connectionString;
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

void MySQLDatabase::startRefreshTableAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;

    if (dbData->loadingTables.load()) {
        return;
    }

    dbData->loadingTables.store(true);
    dbData->tablesFuture =
        std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getTablesWithColumnsAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData || !dbData->loadingTables.load()) {
        return {};
    }

    std::vector<Table> result;

    try {
        if (!dbData->loadingTables.load()) {
            return result;
        }

        // Get table names using the session
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = "SHOW TABLES";
        {
            const auto sql = getSession();
            const soci::rowset tableRs = sql->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!dbData->loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        if (!dbData->loadingTables.load()) {
            return result;
        }

        for (const auto& tableName : tableNames) {
            if (!dbData->loadingTables.load()) {
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
                    if (!dbData->loadingTables.load()) {
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

void MySQLDatabase::refreshViews() {
    if (isLoadingViews()) {
        return;
    }
    startRefreshViewAsync();
}

void MySQLDatabase::startRefreshViewAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;

    if (dbData->loadingViews.load()) {
        return;
    }

    dbData->loadingViews.store(true);
    dbData->viewsFuture =
        std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getViewsWithColumnsAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData || !dbData->loadingViews.load()) {
        return {};
    }

    std::vector<Table> result;

    try {
        if (!dbData->loadingViews.load()) {
            return result;
        }

        // Get view names using the session
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        {
            const auto sql = getSession();
            const soci::rowset viewRs = sql->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!dbData->loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        if (!dbData->loadingViews.load()) {
            return result;
        }

        for (const auto& viewName : viewNames) {
            if (!dbData->loadingViews.load()) {
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
                    if (!dbData->loadingViews.load()) {
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

void MySQLDatabase::refreshSequences() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;

    dbData->sequences.clear();
    dbData->sequencesLoaded = true;
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
            auto indexName = row.get<std::string>(2); // Key_name

            if (!indexMap.contains(indexName)) {
                Index idx;
                idx.name = indexName;
                idx.isUnique = row.get<int>(1) == 0; // Non_unique (0 means unique)
                idx.isPrimary = (indexName == "PRIMARY");
                idx.type = row.get<std::string>(10); // Index_type
                indexMap[indexName] = idx;
            }

            // Add column to the index
            auto colName = row.get<std::string>(4); // Column_name
            indexMap[indexName].columns.push_back(colName);
        }

        // Convert map to vector
        for (auto& idx : indexMap | std::views::values) {
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

std::vector<std::string> MySQLDatabase::getDatabaseNames() {
    if (databasesLoaded) {
        return availableDatabases;
    }

    // Start async loading if not already loading and we're connected
    if (!loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }

    return availableDatabases; // Return current state (maybe empty if still loading)
}

std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>>&
MySQLDatabase::getDatabaseDataMap() {
    // autoload databases if not loaded and not currently loading
    if (!databasesLoaded && !loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void MySQLDatabase::refreshDatabaseNames() {
    if (loadingDatabases.load()) {
        return; // Already loading
    }

    // Clear previous results
    availableDatabases.clear();
    databasesLoaded = false;
    loadingDatabases = true;

    // Start async loading with std::async
    databasesFuture = std::async(std::launch::async, [this]() { return getDatabaseNamesAsync(); });
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

            // Populate databaseDataCache with all available databases
            for (const auto& dbName : availableDatabases) {
                // Use getDatabaseData which creates if not exists
                getDatabaseData(dbName);
            }

            databasesLoaded = true;
            loadingDatabases = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async database loading: " << e.what() << std::endl;
            databasesLoaded = true; // Mark as loaded to prevent retry loops
            loadingDatabases = false;
        }
    }
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

soci::connection_pool*
MySQLDatabase::getConnectionPoolForDatabase(const std::string& dbName) const {
    std::lock_guard lock(sessionMutex);

    // Use nested DatabaseData structure
    const auto* dbData = getDatabaseData(dbName);
    if (dbData && dbData->connectionPool) {
        return dbData->connectionPool.get();
    }
    return nullptr;
}

void MySQLDatabase::initializeConnectionPool(const std::string& dbName,
                                             const std::string& connStr) {
    std::lock_guard lock(sessionMutex);

    // Get or create DatabaseData for this database
    auto* dbData = getDatabaseData(dbName);
    if (!dbData)
        return;

    // Don't recreate if pool already exists
    if (dbData->connectionPool) {
        return;
    }

    constexpr size_t poolSize = 10;
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

    // Store in DatabaseData
    dbData->connectionPool = std::move(pool);
}

std::string MySQLDatabase::buildConnectionString(const std::string& dbName) const {
    std::string connStr = "host=" + connectionInfo.host +
                          " port=" + std::to_string(connectionInfo.port) + " dbname=" + dbName;

    if (!connectionInfo.username.empty()) {
        connStr += " user=" + connectionInfo.username;
    }

    if (!connectionInfo.password.empty()) {
        connStr += " password=" + connectionInfo.password;
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
