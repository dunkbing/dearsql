#include "database/postgresql.hpp"
#include "database/db.hpp"
#include "utils/logger.hpp"
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <vector>

PostgresDatabase::PostgresDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    if (connectionInfo.database.empty()) {
        connectionInfo.database = "postgres";
    }
}

PostgresDatabase::~PostgresDatabase() {
    // Cancel and stop all async operations before cleaning up
    databasesLoader.cancel();
    refreshWorkflow.cancel();

    // Stop all per-database async operations
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (!dbDataPtr)
            continue;
        auto& dbData = *dbDataPtr;
        dbData.schemasLoader.cancel();

        // Wait for all schema-level operations to complete
        for (const auto& schema : dbData.schemas) {
            schema->tablesLoader.cancel();
            schema->viewsLoader.cancel();
            schema->sequencesLoader.cancel();
        }
    }

    PostgresDatabase::disconnect();
}

PostgresDatabaseNode* PostgresDatabase::getDatabaseData(const std::string& dbName) {
    auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        // Create new DatabaseData with the name set
        auto newData = std::make_unique<PostgresDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

const PostgresDatabaseNode* PostgresDatabase::getDatabaseData(const std::string& dbName) const {
    const auto it = databaseDataCache.find(dbName);
    return (it != databaseDataCache.end()) ? it->second.get() : nullptr;
}

std::pair<bool, std::string> PostgresDatabase::connect() {
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);

    try {
        ensureConnectionPoolForDatabase(connectionInfo);
        Logger::info("Successfully connected to PostgreSQL database: " + connectionInfo.database);
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
        auto it = databaseDataCache.find(connectionInfo.database);
        if (it != databaseDataCache.end() && it->second) {
            it->second->connectionPool.reset();
        }
        connected = false;
        std::string error = "Postgres connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void PostgresDatabase::disconnect() {
    std::lock_guard lock(sessionMutex);
    // Clear all connection pools
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->connectionPool.reset();
        }
    }
    connected = false;
}

void PostgresDatabase::refreshConnection() {
    // Start the sequential refresh workflow
    refreshWorkflow.start([this]() -> bool {
        // Step 1: Disconnect and reset state
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        // Step 2: Reconnect (synchronously, without triggering auto-refresh)
        try {
            ensureConnectionPoolForDatabase(connectionInfo);
            Logger::info("Successfully reconnected to PostgreSQL database: " +
                         connectionInfo.database);
            connected = true;
            setLastConnectionError("");
        } catch (const soci::soci_error& e) {
            Logger::error("Reconnection failed: " + std::string(e.what()));
            setLastConnectionError(e.what());
            return false;
        }

        // Step 3: If showAllDatabases is enabled, load database names synchronously
        if (connectionInfo.showAllDatabases) {
            Logger::debug("Loading database names synchronously for refresh...");
            auto databases = getDatabaseNamesAsync();

            // Populate databaseDataCache with all available databases
            for (const auto& dbName : databases) {
                auto it = databaseDataCache.find(dbName);
                if (it == databaseDataCache.end()) {
                    auto newData = std::make_unique<PostgresDatabaseNode>();
                    newData->name = dbName;
                    newData->parentDb = this;
                    databaseDataCache[dbName] = std::move(newData);
                }
            }
            databasesLoaded = true;
        }

        // Step 4: Trigger refresh for all child databases
        Logger::debug("Triggering child database refresh...");
        for (auto& dbDataPtr : databaseDataCache | std::views::values) {
            if (dbDataPtr) {
                Logger::debug(std::format("Refreshing db: {}", dbDataPtr->name));
                dbDataPtr->startSchemasLoadAsync(true, true);
            }
        }

        Logger::info(
            std::format("Refresh workflow completed for {} databases", databaseDataCache.size()));
        return true;
    });
}

QueryResult PostgresDatabase::executeQueryWithResult(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            result.success = false;
            result.errorMessage = "Failed to connect to database: " + error;
            return result;
        }
    }

    try {
        auto sql = getSession();

        const soci::rowset rs = sql->prepare << query;

        // Get column names if available
        const auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                result.columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        // Fetch rows (up to rowLimit)
        int rowCount = 0;
        for (const auto& row : rs) {
            if (rowCount >= rowLimit) {
                break;
            }

            std::vector<std::string> rowData;
            rowData.reserve(row.size());
            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.push_back(convertRowValue(row, i));
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
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    return result;
}

const std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>>&
PostgresDatabase::getDatabaseDataMap() {
    // autoload databases if not loaded and not currently loading
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void PostgresDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }

    databasesLoaded = false;

    // start async loading using AsyncOperation
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool PostgresDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool PostgresDatabase::hasPendingAsyncWork() const {
    // Check connection and database-level async operations
    if (isConnecting() || isLoadingDatabases()) {
        return true;
    }

    // Check all database nodes for pending async work
    for (const auto& [_, dbNode] : databaseDataCache) {
        if (!dbNode) {
            continue;
        }

        // Check if schemas are loading
        if (dbNode->schemasLoader.isRunning()) {
            return true;
        }

        // Check all schema nodes for pending async work
        for (const auto& schema : dbNode->schemas) {
            if (!schema) {
                continue;
            }

            if (schema->tablesLoader.isRunning() || schema->viewsLoader.isRunning() ||
                schema->sequencesLoader.isRunning()) {
                return true;
            }
        }
    }

    return false;
}

void PostgresDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        std::cout << "Async database loading completed. Found " << databases.size() << " databases."
                  << std::endl;

        // Populate databaseDataCache with all available databases
        for (const auto& dbName : databases) {
            auto it = databaseDataCache.find(dbName);
            if (it == databaseDataCache.end()) {
                // Create new DatabaseData with the name set
                auto newData = std::make_unique<PostgresDatabaseNode>();
                newData->name = dbName;
                newData->parentDb = this;
                databaseDataCache[dbName] = std::move(newData);
            }
        }

        databasesLoaded = true;
    });
}

void PostgresDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (success) {
            Logger::info("Refresh workflow completed successfully");
        } else {
            Logger::error("Refresh workflow failed");
        }
    });
}

std::vector<std::string> PostgresDatabase::getDatabaseNamesAsync() const {
    std::vector<std::string> result;

    // Check if we're still supposed to be loading
    if (!databasesLoader.isRunning()) {
        return result;
    }

    try {
        if (!databasesLoader.isRunning()) {
            return result;
        }

        std::vector<std::string> conditions = {sql::eq("datistemplate", "false")};
        if (!connectionInfo.showAllDatabases) {
            conditions.push_back(sql::eq("datname", "'" + connectionInfo.database + "'"));
        }

        const std::string whereClause = sql::and_(conditions);
        const std::string sqlQuery =
            std::format("SELECT datname FROM pg_database WHERE {} ORDER BY datname", whereClause);

        std::cout << "Executing async query to get database names..." << std::endl;
        const auto session = getSession();
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            if (!databasesLoader.isRunning()) {
                break;
            }

            auto dbName = row.get<std::string>(0);
            std::cout << "Found database: " << dbName << std::endl;
            result.push_back(dbName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute async database query: " << e.what() << std::endl;
    }

    Logger::info(std::format("Async query completed. Found: {} databases", result.size()));
    return result;
}

soci::connection_pool*
PostgresDatabase::getConnectionPoolForDatabase(const std::string& dbName) const {
    std::lock_guard lock(sessionMutex);

    // Use nested DatabaseData structure
    auto it = databaseDataCache.find(dbName);
    if (it != databaseDataCache.end() && it->second && it->second->connectionPool) {
        return it->second->connectionPool.get();
    }
    return nullptr;
}

void PostgresDatabase::ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info) {
    std::lock_guard lock(sessionMutex);

    if (info.database.empty()) {
        throw std::runtime_error("ensureConnectionPoolForDatabase: database name is required");
    }

    auto* dbData = getDatabaseData(info.database);
    if (!dbData || dbData->connectionPool) {
        return;
    }

    constexpr size_t poolSize = 3;
    dbData->connectionPool = DatabaseInterface::initializeConnectionPool(info, poolSize);
}

std::unique_ptr<soci::session> PostgresDatabase::getSession() const {
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

void PostgresDatabase::triggerChildDbRefresh() {
    Logger::debug(
        std::format("Triggering child db refresh for connection: {}", connectionInfo.name));

    // loop through all schemas and trigger refresh for tables, views, and sequences
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            Logger::debug(std::format("Refreshing db: {}", dbDataPtr->name));
            dbDataPtr->startSchemasLoadAsync(true, true);
        }
    }

    Logger::info(std::format("Triggered refresh for {} schemas in database {}",
                             databaseDataCache.size(), connectionInfo.name));
}

std::pair<bool, std::string> PostgresDatabase::renameDatabase(const std::string& oldName,
                                                              const std::string& newName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        // PostgreSQL requires that no one is connected to the database being renamed
        // We need to connect to a different database (like postgres) to execute this
        const std::string sql =
            std::format("ALTER DATABASE \"{}\" RENAME TO \"{}\"", oldName, newName);

        auto session = getSession();
        *session << sql;

        // Update the cache if the renamed database exists in it
        auto it = databaseDataCache.find(oldName);
        if (it != databaseDataCache.end()) {
            auto node = std::move(it->second);
            node->name = newName;
            databaseDataCache.erase(it);
            databaseDataCache[newName] = std::move(node);
        }

        Logger::info(std::format("Database '{}' renamed to '{}'", oldName, newName));
        return {true, ""};
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Failed to rename database: {}", e.what()));
        return {false, e.what()};
    }
}

std::pair<bool, std::string> PostgresDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    // Prevent dropping the currently connected database
    if (dbName == connectionInfo.database) {
        return {false, "Cannot drop the currently connected database"};
    }

    try {
        // PostgreSQL requires that no one is connected to the database being dropped
        // First, terminate all connections to the target database
        const std::string terminateSql =
            std::format("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                        "WHERE datname = '{}' AND pid <> pg_backend_pid()",
                        dbName);

        auto session = getSession();
        *session << terminateSql;

        // Now drop the database
        const std::string dropSql = std::format("DROP DATABASE \"{}\"", dbName);
        *session << dropSql;

        // Remove from cache
        databaseDataCache.erase(dbName);

        Logger::info(std::format("Database '{}' dropped successfully", dbName));
        return {true, ""};
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Failed to drop database: {}", e.what()));
        return {false, e.what()};
    }
}
