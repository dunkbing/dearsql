#include "database/mysql.hpp"
#include "utils/logger.hpp"
#include <cctype>
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

            constexpr unsigned int connectTimeoutSeconds = 5;
            mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeoutSeconds);

            // SSL mode
            switch (info.sslmode) {
            case SslMode::Disable: {
                unsigned int mode = SSL_MODE_DISABLED;
                mysql_options(conn, MYSQL_OPT_SSL_MODE, &mode);
                break;
            }
            case SslMode::Require: {
                unsigned int mode = SSL_MODE_REQUIRED;
                mysql_options(conn, MYSQL_OPT_SSL_MODE, &mode);
                break;
            }
            case SslMode::VerifyCA: {
                unsigned int mode = SSL_MODE_VERIFY_CA;
                mysql_options(conn, MYSQL_OPT_SSL_MODE, &mode);
                if (!info.sslCACertPath.empty())
                    mysql_options(conn, MYSQL_OPT_SSL_CA, info.sslCACertPath.c_str());
                break;
            }
            case SslMode::VerifyFull: {
                unsigned int mode = SSL_MODE_VERIFY_IDENTITY;
                mysql_options(conn, MYSQL_OPT_SSL_MODE, &mode);
                if (!info.sslCACertPath.empty())
                    mysql_options(conn, MYSQL_OPT_SSL_CA, info.sslCACertPath.c_str());
                break;
            }
            default: {
                unsigned int mode = SSL_MODE_PREFERRED;
                mysql_options(conn, MYSQL_OPT_SSL_MODE, &mode);
                break;
            }
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

    // Extract a single StatementResult from the current result set on a MYSQL* connection
    StatementResult extractMysqlResult(MYSQL* conn, int rowLimit) {
        StatementResult result;

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

    std::string quoteMysqlIdentifier(const std::string& input) {
        std::string quoted = "`";
        quoted.reserve(input.size() + 2);
        for (char ch : input) {
            quoted.push_back(ch);
            if (ch == '`') {
                quoted.push_back('`');
            }
        }
        quoted.push_back('`');
        return quoted;
    }

    bool isSafeSqlToken(const std::string& input) {
        if (input.empty()) {
            return false;
        }
        for (char ch : input) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (!std::isalnum(uch) && ch != '_') {
                return false;
            }
        }
        return true;
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
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        connected = false;
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }

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
            Logger::warn("MySQLDatabase::disconnect: skipping pool teardown during shutdown "
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

            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(databases);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }

        Logger::info(std::format("MySQL refresh workflow completed for {} databases",
                                 databaseDataCache.size()));
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

        do {
            auto r = extractMysqlResult(conn, rowLimit);
            if (r.success || !r.errorMessage.empty()) {
                result.statements.push_back(std::move(r));
            }
        } while (mysql_next_result(conn) == 0);
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
            Logger::info("MySQL refresh workflow completed successfully");
            std::vector<std::string> refreshedDatabases;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshedDatabases = std::move(pendingRefreshDatabaseNames);
                pendingRefreshDatabaseNames.clear();
            }

            for (const auto& dbName : refreshedDatabases) {
                getDatabaseData(dbName);
            }

            Logger::debug("Ensuring connection pools for all databases...");
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
    if (info.database.empty()) {
        throw std::runtime_error("ensureConnectionPoolForDatabase: database name is required");
    }

    {
        std::lock_guard lock(sessionMutex);
        auto* dbData = getDatabaseData(info.database);
        if (!dbData || dbData->connectionPool) {
            return;
        }
    }

    constexpr size_t poolSize = 10;
    auto newPool = std::make_unique<ConnectionPool<MYSQL*>>(
        poolSize, makeMysqlFactory(info),
        // closer
        [](MYSQL* conn) { mysql_close(conn); },
        // validator
        [](MYSQL* conn) { return mysql_ping(conn) == 0; });

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
            Logger::warn(
                "MySQL database comments are not supported by this backend; ignoring comment");
        }

        auto session = getSession();
        MYSQL* conn = session.get();
        if (mysql_query(conn, sql.c_str()) != 0) {
            std::string err = mysql_error(conn);
            Logger::error(std::format("Failed to create database: {}", err));
            return {false, err};
        }

        Logger::info(std::format("Database '{}' created successfully", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to create database: {}", e.what()));
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
    if (!opts.charset.empty() && !isSafeSqlToken(opts.charset)) {
        return {false, "Invalid charset value"};
    }
    if (!opts.collation.empty() && !isSafeSqlToken(opts.collation)) {
        return {false, "Invalid collation value"};
    }

    try {
        std::string sql = std::format("CREATE DATABASE {}", quoteMysqlIdentifier(opts.name));
        if (!opts.charset.empty())
            sql += std::format(" CHARACTER SET {}", opts.charset);
        if (!opts.collation.empty())
            sql += std::format(" COLLATE {}", opts.collation);
        if (!opts.comment.empty()) {
            Logger::warn(
                "MySQL database comments are not supported by this backend; ignoring comment");
        }

        auto session = getSession();
        MYSQL* conn = session.get();
        if (mysql_query(conn, sql.c_str()) != 0) {
            std::string err = mysql_error(conn);
            Logger::error(std::format("Failed to create database: {}", err));
            return {false, err};
        }

        Logger::info(std::format("Database '{}' created successfully", opts.name));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to create database: {}", e.what()));
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
            // Open a temporary connection to the 'mysql' system database first.
            // Keep the current pool intact until this succeeds so failure paths
            // do not leave the object disconnected.
            auto tempInfo = connectionInfo;
            tempInfo.database = "mysql";
            MYSQL* tempConn = mysql_init(nullptr);
            if (!tempConn) {
                return {false, "mysql_init failed"};
            }
            if (!mysql_real_connect(tempConn, tempInfo.host.c_str(), tempInfo.username.c_str(),
                                    tempInfo.password.c_str(), tempInfo.database.c_str(),
                                    tempInfo.port, nullptr, CLIENT_MULTI_STATEMENTS)) {
                std::string err = mysql_error(tempConn);
                mysql_close(tempConn);
                return {false, std::format("Failed to connect to system database: {}", err)};
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
                Logger::error(std::format("Failed to drop database: {}", err));
                mysql_close(tempConn);

                // Best-effort recovery: restore the original active pool.
                try {
                    ensureConnectionPoolForDatabase(connectionInfo);
                } catch (const std::exception& restoreErr) {
                    Logger::warn(std::format("Failed to restore connection pool for '{}': {}",
                                             originalDb, restoreErr.what()));
                }
                return {false, err};
            }
            mysql_close(tempConn);
        } else {
            auto session = getSession();
            MYSQL* conn = session.get();
            if (mysql_query(conn, sql.c_str()) != 0) {
                std::string err = mysql_error(conn);
                Logger::error(std::format("Failed to drop database: {}", err));
                return {false, err};
            }
        }

        // Remove from cache
        databaseDataCache.erase(dbName);

        if (isDroppingConnectedDb) {
            connectionInfo.database = "mysql";
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
                Logger::warn(switchError);
                return {true, switchError};
            }
        }

        Logger::info(std::format("Database '{}' dropped successfully", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to drop database: {}", e.what()));
        return {false, e.what()};
    }
}
