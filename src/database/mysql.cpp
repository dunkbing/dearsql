#include "database/mysql.hpp"
#include "utils/logger.hpp"
#include <format>
#include <iostream>
#include <mysql/mysql.h>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace {

    struct MysqlResDeleter {
        void operator()(MYSQL_RES* r) const {
            if (r)
                mysql_free_result(r);
        }
    };
    using MysqlResPtr = std::unique_ptr<MYSQL_RES, MysqlResDeleter>;

    // Build a MySQL connection factory from DatabaseConnectionInfo
    std::function<MYSQL*()> makeMysqlFactory(const DatabaseConnectionInfo& info) {
        return [info]() -> MYSQL* {
            MYSQL* conn = mysql_init(nullptr);
            if (!conn) {
                throw std::runtime_error("mysql_init failed");
            }

            // Enable multi-statement support
            unsigned long flags = CLIENT_MULTI_STATEMENTS;

            if (!mysql_real_connect(conn, info.host.c_str(), info.username.c_str(),
                                    info.password.c_str(), info.database.c_str(), info.port,
                                    nullptr, flags)) {
                std::string err = mysql_error(conn);
                mysql_close(conn);
                throw std::runtime_error("MySQL connection failed: " + err);
            }

            // Set character set
            mysql_set_character_set(conn, "utf8mb4");

            return conn;
        };
    }

    // Extract a single QueryResult from the current result set on a MYSQL* connection
    QueryResult extractMysqlResult(MYSQL* conn, int rowLimit) {
        QueryResult result;

        MYSQL_RES* rawRes = mysql_store_result(conn);
        if (rawRes) {
            MysqlResPtr res(rawRes);
            unsigned int nFields = mysql_num_fields(res.get());
            MYSQL_FIELD* fields = mysql_fetch_fields(res.get());

            for (unsigned int i = 0; i < nFields; i++) {
                result.columnNames.emplace_back(fields[i].name);
            }

            int rowCount = 0;
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr && rowCount < rowLimit) {
                unsigned long* lengths = mysql_fetch_lengths(res.get());
                std::vector<std::string> rowData;
                rowData.reserve(nFields);
                for (unsigned int i = 0; i < nFields; i++) {
                    if (row[i] == nullptr) {
                        rowData.emplace_back("NULL");
                    } else {
                        rowData.emplace_back(row[i], lengths[i]);
                    }
                }
                result.tableData.push_back(std::move(rowData));
                rowCount++;
            }

            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            my_ulonglong totalRows = mysql_num_rows(res.get());
            if (static_cast<int>(totalRows) >= rowLimit) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
            result.success = true;
        } else {
            // No result set - could be DML/DDL or error
            if (mysql_field_count(conn) == 0) {
                // DML/DDL statement
                my_ulonglong affected = mysql_affected_rows(conn);
                if (affected != (my_ulonglong)-1) {
                    result.message = std::format("{} row(s) affected", affected);
                } else {
                    result.message = "Query executed successfully";
                }
                result.success = true;
            } else {
                // Error
                result.success = false;
                result.errorMessage = mysql_error(conn);
            }
        }

        return result;
    }

} // namespace

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
    } catch (const std::exception& e) {
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
        } catch (const std::exception& e) {
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

std::vector<QueryResult> MySQLDatabase::executeQuery(const std::string& query, int rowLimit) {
    std::vector<QueryResult> results;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connect().first) {
        QueryResult r;
        r.success = false;
        r.errorMessage = "Not connected to database";
        results.push_back(r);
        return results;
    }

    try {
        auto session = getSession();
        MYSQL* conn = session.get();

        if (mysql_query(conn, query.c_str()) != 0) {
            QueryResult r;
            r.success = false;
            r.errorMessage = mysql_error(conn);
            results.push_back(r);
            return results;
        }

        do {
            auto r = extractMysqlResult(conn, rowLimit);
            const auto endTime = std::chrono::high_resolution_clock::now();
            r.executionTimeMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            if (r.success || !r.errorMessage.empty()) {
                results.push_back(std::move(r));
            }
        } while (mysql_next_result(conn) == 0);
    } catch (const std::exception& e) {
        QueryResult r;
        r.success = false;
        r.errorMessage = e.what();
        results.push_back(r);
    }

    return results;
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
        if (!isConnected()) {
            std::cerr << "Cannot load databases: not connected" << std::endl;
            return result;
        }

        // If showAllDatabases is false, only return the current database
        if (!connectionInfo.showAllDatabases) {
            result.push_back(connectionInfo.database);
            return result;
        }

        auto session = getSession();
        MYSQL* conn = session.get();

        if (mysql_query(conn, "SHOW DATABASES") != 0) {
            std::cerr << "Failed to get databases: " << mysql_error(conn) << std::endl;
            return result;
        }

        MysqlResPtr res(mysql_store_result(conn));
        if (!res) {
            return result;
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get())) != nullptr) {
            std::string dbName = row[0] ? row[0] : "";
            // Filter out system databases
            if (dbName != "information_schema" && dbName != "performance_schema" &&
                dbName != "mysql" && dbName != "sys") {
                result.push_back(dbName);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to execute async database query: " << e.what() << std::endl;
    }

    Logger::info(std::format("Async query completed. Found {} databases", result.size()));
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
    dbData->connectionPool = std::make_unique<ConnectionPool<MYSQL*>>(
        poolSize, makeMysqlFactory(info),
        // closer
        [](MYSQL* conn) { mysql_close(conn); },
        // validator
        [](MYSQL* conn) { return mysql_ping(conn) == 0; });
}

ConnectionPool<MYSQL*>::Session MySQLDatabase::getSession() const {
    std::lock_guard lock(sessionMutex);

    const std::string targetDb = connectionInfo.database;

    // Find connection pool in databaseDataCache
    auto it = databaseDataCache.find(targetDb);
    if (it == databaseDataCache.end() || !it->second || !it->second->connectionPool) {
        throw std::runtime_error(
            "MySQLDatabase::getSession: Connection pool not available for database: " + targetDb);
    }

    return it->second->connectionPool->acquire();
}

std::pair<bool, std::string> MySQLDatabase::renameDatabase(const std::string& oldName,
                                                           const std::string& newName) {
    // MySQL does not support direct database renaming
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
        MYSQL* conn = session.get();
        if (mysql_query(conn, sql.c_str()) != 0) {
            std::string err = mysql_error(conn);
            Logger::error(std::format("Failed to drop database: {}", err));
            return {false, err};
        }

        // Remove from cache
        databaseDataCache.erase(dbName);

        Logger::info(std::format("Database '{}' dropped successfully", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to drop database: {}", e.what()));
        return {false, e.what()};
    }
}
