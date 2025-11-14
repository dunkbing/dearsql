#include "database/mysql.hpp"
#include "database/db.hpp"
#include "utils/logger.hpp"
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
    if (database.empty()) {
        this->database = "mysql";
    }
}

MySQLDatabase::~MySQLDatabase() {
    // Stop all async operations before cleaning up
    databasesLoader.cancel();
    refreshWorkflow.cancel();

    // Stop all per-database async operations
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->tablesLoader.cancel();
            dbDataPtr->viewsLoader.cancel();
        }
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
        newData->ensureConnectionPool();
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
        newData->ensureConnectionPool();
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> MySQLDatabase::connect() {
    const auto* pool = getConnectionPoolForDatabase(database);
    if (connected && pool) {
        return {true, ""};
    }

    setAttemptedConnection(true);

    try {
        initializeConnectionPool(database, connectionString);
        Logger::info("Successfully connected to MySQL database: " + database);
        connected = true;
        setLastConnectionError("");

        // Start loading databases immediately if showAllDatabases is enabled
        if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning()) {
            Logger::debug("Starting async database loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Connection to database failed: {}", e.what()));
        std::lock_guard lock(sessionMutex);
        // Clear connection pool from DatabaseData
        auto* dbData = getDatabaseData(database);
        if (dbData) {
            dbData->connectionPool.reset();
        }
        connected = false;
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

void MySQLDatabase::refreshConnection() {
    // Start the sequential refresh workflow
    refreshWorkflow.start([this]() -> bool {
        // Step 1: Disconnect and reset state
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        // Step 2: Reconnect (synchronously, without triggering auto-refresh)
        try {
            initializeConnectionPool(database, connectionString);
            Logger::info("Successfully reconnected to MySQL database: " + database);
            connected = true;
            setLastConnectionError("");
        } catch (const soci::soci_error& e) {
            Logger::error("MySQL reconnection failed: " + std::string(e.what()));
            setLastConnectionError(e.what());
            return false;
        }

        // Step 3: If showAllDatabases is enabled, load database names synchronously
        if (connectionInfo.showAllDatabases) {
            Logger::debug("Loading database names synchronously for refresh...");
            auto databases = getDatabaseNamesAsync();

            // Populate databaseDataCache with all available databases
            for (const auto& dbName : databases) {
                getDatabaseData(dbName);
            }
        }

        // Step 4: Ensure all database nodes have connection pools
        Logger::debug("Ensuring connection pools for all databases...");
        for (auto& dbDataPtr : databaseDataCache | std::views::values) {
            if (dbDataPtr) {
                dbDataPtr->ensureConnectionPool();
            }
        }
        databasesLoaded = true;

        // Step 5: Trigger refresh for all child databases
        Logger::debug("Triggering child database refresh...");
        for (auto& dbDataPtr : databaseDataCache | std::views::values) {
            if (dbDataPtr) {
                Logger::debug(std::format("Refreshing db: {}", dbDataPtr->name));
                dbDataPtr->startTablesLoadAsync(true);
                dbDataPtr->startViewsLoadAsync(true);
            }
        }

        Logger::info(std::format("MySQL refresh workflow completed for {} databases",
                                 databaseDataCache.size()));
        return true;
    });
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

    // Delegate to MySQLDatabaseNode
    dbData->startTablesLoadAsync();
}

void MySQLDatabase::startRefreshViewAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;

    // Delegate to MySQLDatabaseNode
    dbData->startViewsLoadAsync();
}

std::vector<Table> MySQLDatabase::getViewsWithColumnsAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData) {
        return {};
    }

    // Delegate to MySQLDatabaseNode
    return dbData->getViewsForDatabaseAsync();
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

std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>>&
MySQLDatabase::getDatabaseDataMap() {
    // autoload databases if not loaded and not currently loading
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void MySQLDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return; // Already loading
    }

    // Clear previous results
    databasesLoaded = false;

    // Start async loading using AsyncOperation
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool MySQLDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

void MySQLDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        std::cout << "Async database loading completed. Found " << databases.size() << " databases."
                  << std::endl;

        // Populate databaseDataCache with all available databases
        for (const auto& dbName : databases) {
            // Use getDatabaseData which creates if not exists
            getDatabaseData(dbName);
        }

        databasesLoaded = true;
    });
}

void MySQLDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](bool success) {
        if (success) {
            Logger::info("MySQL refresh workflow completed successfully");
        } else {
            Logger::error("MySQL refresh workflow failed");
        }
    });
}

std::vector<std::string> MySQLDatabase::getDatabaseNamesAsync() const {
    Logger::info("getDatabaseNamesAsync");
    std::vector<std::string> result;

    try {
        // Check if we have a valid connection pool before trying to query
        if (!isConnected()) {
            std::cerr << "Cannot load databases: not connected" << std::endl;
            return result;
        }

        std::cout << "DEBUG: isConnected() = true, attempting to get session for database: "
                  << database << std::endl;

        // If showAllDatabases is false, only return the current database
        if (!connectionInfo.showAllDatabases) {
            result.push_back(database);
            std::cout << "showAllDatabases is false, returning only current database: " << database
                      << std::endl;
            return result;
        }

        const std::string sqlQuery = "SHOW DATABASES";

        std::cout << "Executing async query to get database names..." << std::endl;
        const auto sql = getSession();
        std::cout << "DEBUG: Session obtained successfully" << std::endl;
        const soci::rowset rs = sql->prepare << sqlQuery;

        for (const auto& row : rs) {
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
    auto it = databaseDataCache.find(dbName);
    if (it != databaseDataCache.end() && it->second && it->second->connectionPool) {
        return it->second->connectionPool.get();
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
        throw std::runtime_error(
            "MySQLDatabase::getSession: Connection pool not available for database: " + targetDb);
    }
    auto res = std::make_unique<soci::session>(*pool);
    if (!res->is_connected()) {
        res->reconnect();
    }
    return res;
}
