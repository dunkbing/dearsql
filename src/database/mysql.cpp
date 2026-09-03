#include "database/mysql.hpp"
#include "database/ddl_utils.hpp"
#include "database/mysql/mysql_internal.hpp"
#include "database/sql_builder.hpp"
#include <cctype>
#include <format>
#include <iostream>
#include <mysql/mysql.h>
#include <ranges>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <vector>

using mysql_internal::extractMysqlResult;
using mysql_internal::makeMysqlFactory;
using mysql_internal::MysqlResPtr;

MySQLDatabase::MySQLDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    spdlog::debug("DEBUG: Creating MySQLDatabase with database = '{}', showAllDatabases = {}",
                  connectionInfo.database, connInfo.showAllDatabases);
    // empty database is allowed: connect without a default schema so users
    // lacking grants on the `mysql` system db can still connect (#19)
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
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        connected = false;
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }

    try {
        ensureConnectionPoolForDatabase(connectionInfo);
        spdlog::debug("Successfully connected to MySQL database: {}", connectionInfo.database);
        connected = true;
        setLastConnectionError("");

        // Start loading databases immediately if showAllDatabases is enabled.
        // With no default database there is nothing else to show, so list too.
        if ((connectionInfo.showAllDatabases || connectionInfo.database.empty()) &&
            !databasesLoaded && !databasesLoader.isRunning()) {
            spdlog::debug("Starting async database loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Connection to database failed: {}", e.what());
        std::lock_guard lock(sessionMutex);
        // Clear connection pool if it exists — avoid getDatabaseData() here
        // because it calls ensureConnectionPool() which would throw again.
        auto it = databaseDataCache.find(connectionInfo.database);
        if (it != databaseDataCache.end() && it->second) {
            it->second->connectionPool.reset();
        }
        connected = false;
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MySQLDatabase::disconnect() {
    if (AsyncOperationControl::skipWaitOnDestroy().load(std::memory_order_relaxed)) {
        std::unique_lock lock(sessionMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            spdlog::warn("MySQLDatabase::disconnect: skipping pool teardown during shutdown "
                         "(connection setup still in progress)");
            connected = false;
            return;
        }

        // Clear all connection pools
        for (auto& dbDataPtr : databaseDataCache | std::views::values) {
            if (dbDataPtr) {
                dbDataPtr->connectionPool.reset();
            }
        }
        stopSshTunnel();
        connected = false;
        return;
    }

    std::lock_guard lock(sessionMutex);
    // Clear all connection pools
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->connectionPool.reset();
        }
    }
    stopSshTunnel();
    connected = false;
}

void MySQLDatabase::refreshConnection() {
    getDatabaseData(connectionInfo.database);

    // Start the sequential refresh workflow
    refreshWorkflow.start([this]() -> bool {
        // Step 1: Disconnect and reset state
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        // Step 2: Reconnect (synchronously, without triggering auto-refresh)
        try {
            ensureConnectionPoolForDatabase(connectionInfo);
            spdlog::debug("Successfully reconnected to MySQL database: {}",
                          connectionInfo.database);
            connected = true;
            setLastConnectionError("");
        } catch (const std::exception& e) {
            spdlog::error("MySQL reconnection failed: {}", e.what());
            setLastConnectionError(e.what());
            return false;
        }

        // Step 3: If showAllDatabases is enabled, load database names synchronously
        if (connectionInfo.showAllDatabases || connectionInfo.database.empty()) {
            spdlog::debug("Loading database names synchronously for refresh...");
            auto databases = getDatabaseNamesAsync();

            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(databases);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }

        spdlog::debug("MySQL refresh workflow completed for {} databases",
                      databaseDataCache.size());
        return true;
    });
}

QueryResult MySQLDatabase::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connect().first) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Not connected to database";
        result.statements.push_back(r);
        return result;
    }

    try {
        auto session = getSession();
        MYSQL* conn = session.get();

        if (mysql_query(conn, query.c_str()) != 0) {
            StatementResult r;
            r.success = false;
            r.errorMessage = mysql_error(conn);
            result.statements.push_back(r);
            return result;
        }

        // See MySQLDatabaseNode::executeQueryOn: >0 is an error, not end of results.
        int next = 0;
        do {
            auto r = extractMysqlResult(conn, rowLimit);
            if (r.success || !r.errorMessage.empty()) {
                result.statements.push_back(std::move(r));
            }
            next = mysql_next_result(conn);
        } while (next == 0);

        if (next > 0) {
            StatementResult r;
            r.success = false;
            r.errorMessage = mysql_error(conn);
            result.statements.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        StatementResult r;
        r.success = false;
        r.errorMessage = e.what();
        result.statements.push_back(r);
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
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
            spdlog::debug("MySQL refresh workflow completed successfully");
            std::vector<std::string> refreshedDatabases;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshedDatabases = std::move(pendingRefreshDatabaseNames);
                pendingRefreshDatabaseNames.clear();
            }

            for (const auto& dbName : refreshedDatabases) {
                getDatabaseData(dbName);
            }

            spdlog::debug("Ensuring connection pools for all databases...");
            for (auto& dbDataPtr : databaseDataCache | std::views::values) {
                if (dbDataPtr) {
                    dbDataPtr->ensureConnectionPool();
                }
            }

            databasesLoaded = true;

            // Trigger child refresh on the main thread to avoid data races
            for (auto& [_, dbDataPtr] : databaseDataCache) {
                if (dbDataPtr) {
                    dbDataPtr->startTablesLoadAsync(true);
                    dbDataPtr->startViewsLoadAsync(true);
                }
            }
        } else {
            spdlog::error("MySQL refresh workflow failed");
        }
    });
}

std::vector<std::string> MySQLDatabase::getDatabaseNamesAsync() const {
    spdlog::debug("getDatabaseNamesAsync");
    std::vector<std::string> result;

    try {
        if (!isConnected()) {
            std::cerr << "Cannot load databases: not connected" << std::endl;
            return result;
        }

        // If showAllDatabases is false, only return the current database.
        // With no current database, fall through and list what grants allow.
        if (!connectionInfo.showAllDatabases && !connectionInfo.database.empty()) {
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

    spdlog::debug("Async query completed. Found {} databases", result.size());
    return result;
}

void MySQLDatabase::ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info) {
    // empty database = server-level pool with no default schema
    {
        std::lock_guard lock(sessionMutex);
        auto* dbData = getDatabaseData(info.database);
        if (!dbData || dbData->connectionPool) {
            return;
        }
    }

    auto newPool = std::make_unique<ConnectionPool<MYSQL*>>(
        makeMysqlFactory(info),
        // closer
        [](MYSQL* conn) { mysql_close(conn); },
        // validator
        [](MYSQL* conn) { return mysql_ping(conn) == 0; },
        mysql_internal::makeMysqlCanceller(info));

    std::lock_guard lock(sessionMutex);
    auto* dbData = getDatabaseData(info.database);
    if (!dbData || dbData->connectionPool) {
        return;
    }
    dbData->connectionPool = std::move(newPool);
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

std::pair<bool, std::string> MySQLDatabase::createDatabase(const std::string& dbName,
                                                           const std::string& comment) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    if (dbName.empty()) {
        return {false, "Database name cannot be empty"};
    }

    try {
        std::string sql = std::format("CREATE DATABASE `{}`", dbName);
        if (!comment.empty()) {
            spdlog::warn(
                "MySQL database comments are not supported by this backend; ignoring comment");
        }

        auto session = getSession();
        MYSQL* conn = session.get();
        if (mysql_query(conn, sql.c_str()) != 0) {
            std::string err = mysql_error(conn);
            spdlog::error("Failed to create database: {}", err);
            return {false, err};
        }

        spdlog::debug("Database '{}' created successfully", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Failed to create database: {}", e.what());
        return {false, e.what()};
    }
}

std::pair<bool, std::string>
MySQLDatabase::createDatabaseWithOptions(const CreateDatabaseOptions& opts) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    if (opts.name.empty()) {
        return {false, "Database name cannot be empty"};
    }
    if (!opts.charset.empty() && !ddl_utils::isSafeSqlToken(opts.charset)) {
        return {false, "Invalid charset value"};
    }
    if (!opts.collation.empty() && !ddl_utils::isSafeSqlToken(opts.collation)) {
        return {false, "Invalid collation value"};
    }

    try {
        const auto builder = createSQLBuilder(DatabaseType::MYSQL);
        std::string sql = std::format("CREATE DATABASE {}", builder->quoteIdentifier(opts.name));
        if (!opts.charset.empty())
            sql += std::format(" CHARACTER SET {}", opts.charset);
        if (!opts.collation.empty())
            sql += std::format(" COLLATE {}", opts.collation);
        if (!opts.comment.empty()) {
            spdlog::warn(
                "MySQL database comments are not supported by this backend; ignoring comment");
        }

        auto session = getSession();
        MYSQL* conn = session.get();
        if (mysql_query(conn, sql.c_str()) != 0) {
            std::string err = mysql_error(conn);
            spdlog::error("Failed to create database: {}", err);
            return {false, err};
        }

        spdlog::debug("Database '{}' created successfully", opts.name);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Failed to create database: {}", e.what());
        return {false, e.what()};
    }
}

std::pair<bool, std::string> MySQLDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        const std::string sql = std::format("DROP DATABASE `{}`", dbName);
        const std::string originalDb = connectionInfo.database;
        const bool isDroppingConnectedDb = (dbName == originalDb);

        if (isDroppingConnectedDb) {
            // Open a temporary connection with no default schema first — the
            // user may lack grants on the `mysql` system db (#19). Keep the
            // current pool intact until this succeeds so failure paths do not
            // leave the object disconnected.
            MYSQL* tempConn = mysql_init(nullptr);
            if (!tempConn) {
                return {false, "mysql_init failed"};
            }
            constexpr unsigned int tcpProtocol = MYSQL_PROTOCOL_TCP;
            mysql_options(tempConn, MYSQL_OPT_PROTOCOL, &tcpProtocol);
            if (!mysql_real_connect(tempConn, connectionInfo.host.c_str(),
                                    connectionInfo.username.c_str(),
                                    connectionInfo.password.c_str(), nullptr, connectionInfo.port,
                                    nullptr, CLIENT_MULTI_STATEMENTS)) {
                std::string err = mysql_error(tempConn);
                mysql_close(tempConn);
                return {false, std::format("Failed to open maintenance connection: {}", err)};
            }

            // Destroy the connection pool for the target database so active
            // sessions are closed before DROP DATABASE.
            {
                std::lock_guard lock(sessionMutex);
                auto it = databaseDataCache.find(dbName);
                if (it != databaseDataCache.end() && it->second) {
                    it->second->connectionPool.reset();
                }
            }

            if (mysql_query(tempConn, sql.c_str()) != 0) {
                std::string err = mysql_error(tempConn);
                spdlog::error("Failed to drop database: {}", err);
                mysql_close(tempConn);

                // Best-effort recovery: restore the original active pool.
                try {
                    ensureConnectionPoolForDatabase(connectionInfo);
                } catch (const std::exception& restoreErr) {
                    spdlog::warn("Failed to restore connection pool for '{}': {}", originalDb,
                                 restoreErr.what());
                }
                return {false, err};
            }
            mysql_close(tempConn);
        } else {
            auto session = getSession();
            MYSQL* conn = session.get();
            if (mysql_query(conn, sql.c_str()) != 0) {
                std::string err = mysql_error(conn);
                spdlog::error("Failed to drop database: {}", err);
                return {false, err};
            }
        }

        // Remove from cache
        databaseDataCache.erase(dbName);

        if (isDroppingConnectedDb) {
            connectionInfo.database = ""; // no default schema (#19)
            try {
                ensureConnectionPoolForDatabase(connectionInfo);
                connected = true;
                setLastConnectionError("");
            } catch (const std::exception& switchErr) {
                connected = false;
                const std::string switchError = std::format(
                    "Database dropped, but failed to switch active connection to mysql: {}",
                    switchErr.what());
                setLastConnectionError(switchError);
                spdlog::warn(switchError);
                return {true, switchError};
            }
        }

        spdlog::debug("Database '{}' dropped successfully", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Failed to drop database: {}", e.what());
        return {false, e.what()};
    }
}
