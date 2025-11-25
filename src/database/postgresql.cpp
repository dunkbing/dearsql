#include "database/postgresql.hpp"
#include "database/db.hpp"
#include "utils/logger.hpp"
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <sstream>
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

// Helper methods for per-database data access
PostgresDatabaseNode* PostgresDatabase::getCurrentDatabaseData() {
    auto it = databaseDataCache.find(connectionInfo.database);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<PostgresDatabaseNode>();
        newData->name = connectionInfo.database;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[connectionInfo.database] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

const PostgresDatabaseNode* PostgresDatabase::getCurrentDatabaseData() const {
    const auto it = databaseDataCache.find(connectionInfo.database);
    return (it != databaseDataCache.end()) ? it->second.get() : nullptr;
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
    const auto* pool = getConnectionPoolForDatabase(connectionInfo.database);
    if (connected && pool) {
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

std::string PostgresDatabase::executeQuery(const std::string& query) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            return "Error: Failed to connect to database: " + error;
        }
    }

    try {
        auto sql = getSession();
        std::stringstream output;

        // Check if this is a DDL statement (ALTER, CREATE, DROP, etc.)
        std::string upperQuery = query;
        std::ranges::transform(upperQuery, upperQuery.begin(), ::toupper);
        const bool isDDL = upperQuery.find("ALTER ") == 0 || upperQuery.find("CREATE ") == 0 ||
                           upperQuery.find("DROP ") == 0 || upperQuery.find("COMMENT ") == 0;

        if (isDDL) {
            // Use once() for DDL statements
            *sql << query;
            output << "Query executed successfully.";
        } else {
            // Use prepare for SELECT and other DML statements
            const soci::rowset rs = sql->prepare << query;

            // Get column names if available
            const auto it = rs.begin();
            if (it != rs.end()) {
                const soci::row& firstRow = *it;
                for (std::size_t i = 0; i < firstRow.size(); ++i) {
                    output << firstRow.get_properties(i).get_name();
                    if (i < firstRow.size() - 1)
                        output << " | ";
                }
                output << "\n";

                for (std::size_t i = 0; i < firstRow.size(); ++i) {
                    output << "----------";
                    if (i < firstRow.size() - 1)
                        output << "-+-";
                }
                output << "\n";
            }

            int rowCount = 0;
            for (const auto& row : rs) {
                if (rowCount >= 1000)
                    break;
                for (std::size_t i = 0; i < row.size(); ++i) {
                    output << convertRowValue(row, i);
                    if (i < row.size() - 1)
                        output << " | ";
                }
                output << "\n";
                rowCount++;
            }

            if (rowCount == 0) {
                output << "Query executed successfully.";
            } else if (rowCount == 1000) {
                output << "\n... (showing first 1000 rows)";
            }
        }

        return output.str();
    } catch (const soci::soci_error& e) {
        return "Error: " + std::string(e.what());
    }
}

bool PostgresDatabase::isLoadingSchemas() const {
    const auto* dbData = getCurrentDatabaseData();
    return dbData ? dbData->schemasLoader.isRunning() : false;
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
    dbData->connectionPool = BaseDatabaseImpl::initializeConnectionPool(info, poolSize);
}

std::unique_ptr<soci::session> PostgresDatabase::getSession(const std::string& dbName) const {
    const std::string targetDb = dbName.empty() ? connectionInfo.database : dbName;
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
