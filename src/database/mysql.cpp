#include "database/mysql.hpp"
#include "utils/logger.hpp"
#include <format>
#include <iostream>
#include <ranges>
#include <unordered_map>
#include <vector>

MySQLDatabase::MySQLDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    Logger::debug(
        std::format("DEBUG: Creating MySQLDatabase with database = '{}', showAllDatabases = {}",
                    connectionInfo.database, connInfo.showAllDatabases));
    if (connectionInfo.database.empty()) {
        connectionInfo.database = "mysql";
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
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);

    try {
        ensureConnectionPoolForDatabase(connectionInfo);
        Logger::info("Successfully connected to MySQL database: " + connectionInfo.database);
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
        auto* dbData = getDatabaseData(connectionInfo.database);
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
            ensureConnectionPoolForDatabase(connectionInfo);
            Logger::info("Successfully reconnected to MySQL database: " + connectionInfo.database);
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

QueryResult MySQLDatabase::executeQueryWithResult(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connect().first) {
        result.success = false;
        result.errorMessage = "Not connected to database";
        return result;
    }

    try {
        const auto sql = getSession();
        const soci::rowset rs = (sql->prepare << query);

        // Get column names and fetch rows
        bool firstRow = true;
        int rowCount = 0;

        for (auto& row : rs) {
            if (firstRow) {
                for (std::size_t i = 0; i != row.size(); ++i) {
                    result.columnNames.push_back(row.get_properties(i).get_name());
                }
                firstRow = false;
            }

            if (rowCount >= rowLimit) {
                break;
            }

            std::vector<std::string> rowData;
            rowData.reserve(row.size());
            for (std::size_t i = 0; i != row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                } else {
                    rowData.push_back(row.get<std::string>(i, ""));
                }
            }
            result.tableData.push_back(rowData);
            rowCount++;
        }

        // Set message based on result
        if (!result.columnNames.empty()) {
            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            if (result.tableData.size() >= static_cast<size_t>(rowLimit)) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
        } else {
            result.message = "Query executed successfully";
        }

        result.success = true;
    } catch (const soci::soci_error& e) {
        result.success = false;
        result.errorMessage = e.what();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    return result;
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

bool MySQLDatabase::hasPendingAsyncWork() const {
    // Check connection and database-level async operations
    if (isConnecting() || isLoadingDatabases()) {
        return true;
    }

    // Check all database nodes for pending async work
    for (const auto& [_, dbNode] : databaseDataCache) {
        if (!dbNode) {
            continue;
        }

        // Check if tables, views, or sequences are loading
        if (dbNode->tablesLoader.isRunning() || dbNode->viewsLoader.isRunning()) {
            return true;
        }
    }

    return false;
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
    refreshWorkflow.check([this](const bool success) {
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
                  << connectionInfo.database << std::endl;

        // If showAllDatabases is false, only return the current database
        if (!connectionInfo.showAllDatabases) {
            result.push_back(connectionInfo.database);
            std::cout << "showAllDatabases is false, returning only current database: "
                      << connectionInfo.database << std::endl;
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

void MySQLDatabase::ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info) {
    std::lock_guard lock(sessionMutex);

    if (info.database.empty()) {
        throw std::runtime_error("ensureConnectionPoolForDatabase: database name is required");
    }

    auto* dbData = getDatabaseData(info.database);
    if (!dbData || dbData->connectionPool) {
        return;
    }

    constexpr size_t poolSize = 10;
    dbData->connectionPool = DatabaseInterface::initializeConnectionPool(info, poolSize);
}

std::unique_ptr<soci::session> MySQLDatabase::getSession() const {
    std::lock_guard lock(sessionMutex);

    const std::string targetDb = connectionInfo.database;

    // Find connection pool in databaseDataCache
    auto it = databaseDataCache.find(targetDb);
    if (it == databaseDataCache.end() || !it->second || !it->second->connectionPool) {
        throw std::runtime_error(
            "MySQLDatabase::getSession: Connection pool not available for database: " + targetDb);
    }

    auto res = std::make_unique<soci::session>(*it->second->connectionPool);
    if (!res->is_connected()) {
        res->reconnect();
    }
    return res;
}

std::pair<bool, std::string> MySQLDatabase::renameDatabase(const std::string& oldName,
                                                           const std::string& newName) {
    // MySQL does not support direct database renaming
    // This would require creating a new database, copying all tables, and dropping the old one
    return {false, "MySQL does not support direct database renaming. "
                   "You need to create a new database, copy all data, and drop the old one."};
}

std::pair<bool, std::string> MySQLDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    // Prevent dropping the currently connected database
    if (dbName == connectionInfo.database) {
        return {false, "Cannot drop the currently connected database"};
    }

    try {
        const std::string sql = std::format("DROP DATABASE `{}`", dbName);

        auto session = getSession();
        *session << sql;

        // Remove from cache
        databaseDataCache.erase(dbName);

        Logger::info(std::format("Database '{}' dropped successfully", dbName));
        return {true, ""};
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Failed to drop database: {}", e.what()));
        return {false, e.what()};
    }
}
