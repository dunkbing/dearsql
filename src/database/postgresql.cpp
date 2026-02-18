#include "database/postgresql.hpp"
#include "database/db.hpp"
#include "utils/logger.hpp"
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace {

    struct PgResultDeleter {
        void operator()(PGresult* r) const {
            if (r)
                PQclear(r);
        }
    };
    using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

    std::string pgValue(PGresult* res, int row, int col) {
        if (PQgetisnull(res, row, col)) {
            return "NULL";
        }
        return PQgetvalue(res, row, col);
    }

    // Build a libpq connection string from DatabaseConnectionInfo
    std::string buildPqConnStr(const DatabaseConnectionInfo& info) {
        std::string connStr = "host=" + info.host + " port=" + std::to_string(info.port);
        if (!info.database.empty()) {
            connStr += " dbname=" + info.database;
        } else {
            connStr += " dbname=postgres";
        }
        if (!info.username.empty()) {
            connStr += " user=" + info.username;
        }
        if (!info.password.empty()) {
            connStr += " password=" + info.password;
        }
        if (!info.sslmode.empty()) {
            connStr += " sslmode=" + info.sslmode;
        }
        return connStr;
    }

    // Extract a single QueryResult from a PGresult
    QueryResult extractPgResult(PGresult* res, int rowLimit) {
        QueryResult result;
        ExecStatusType status = PQresultStatus(res);

        if (status == PGRES_TUPLES_OK) {
            int nFields = PQnfields(res);
            int nRows = PQntuples(res);

            for (int col = 0; col < nFields; col++) {
                result.columnNames.emplace_back(PQfname(res, col));
            }

            int limit = std::min(nRows, rowLimit);
            for (int row = 0; row < limit; row++) {
                std::vector<std::string> rowData;
                rowData.reserve(nFields);
                for (int col = 0; col < nFields; col++) {
                    rowData.push_back(pgValue(res, row, col));
                }
                result.tableData.push_back(std::move(rowData));
            }

            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            if (nRows >= rowLimit) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
            result.success = true;
        } else if (status == PGRES_COMMAND_OK) {
            const char* affected = PQcmdTuples(res);
            if (affected && *affected) {
                result.message = std::format("{} row(s) affected", affected);
            } else {
                result.message = "Query executed successfully";
            }
            result.success = true;
        } else {
            result.success = false;
            result.errorMessage = PQresultErrorMessage(res);
        }
        return result;
    }

    std::string escapeSingleQuotes(const std::string& input) {
        std::string escaped;
        escaped.reserve(input.size());
        for (char ch : input) {
            escaped.push_back(ch);
            if (ch == '\'') {
                escaped.push_back('\'');
            }
        }
        return escaped;
    }

} // namespace

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
    } catch (const std::exception& e) {
        Logger::error(std::format("Connection to database failed: {}", e.what()));
        std::lock_guard lock(sessionMutex);
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
        } catch (const std::exception& e) {
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

std::vector<QueryResult> PostgresDatabase::executeQuery(const std::string& query, int rowLimit) {
    std::vector<QueryResult> results;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            QueryResult r;
            r.success = false;
            r.errorMessage = "Failed to connect to database: " + error;
            results.push_back(r);
            return results;
        }
    }

    try {
        auto session = getSession();
        PGconn* conn = session.get();

        if (!PQsendQuery(conn, query.c_str())) {
            QueryResult r;
            r.success = false;
            r.errorMessage = PQerrorMessage(conn);
            results.push_back(r);
            return results;
        }

        while (PGresult* raw = PQgetResult(conn)) {
            PgResultPtr res(raw);
            auto r = extractPgResult(res.get(), rowLimit);
            const auto endTime = std::chrono::high_resolution_clock::now();
            r.executionTimeMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            // Only add meaningful results (skip empty pipeline results)
            if (r.success || !r.errorMessage.empty()) {
                results.push_back(std::move(r));
            }
        }
    } catch (const std::exception& e) {
        QueryResult r;
        r.success = false;
        r.errorMessage = e.what();
        results.push_back(r);
    }

    return results;
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
        auto session = getSession();
        PGconn* conn = session.get();
        PgResultPtr res(PQexec(conn, sqlQuery.c_str()));
        if (!res || PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
            std::cerr << "Failed to execute async database query: " << PQerrorMessage(conn)
                      << std::endl;
            return result;
        }

        int nRows = PQntuples(res.get());
        for (int i = 0; i < nRows; i++) {
            if (!databasesLoader.isRunning()) {
                break;
            }
            auto dbName = std::string(PQgetvalue(res.get(), i, 0));
            std::cout << "Found database: " << dbName << std::endl;
            result.push_back(dbName);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to execute async database query: " << e.what() << std::endl;
    }

    Logger::info(std::format("Async query completed. Found: {} databases", result.size()));
    return result;
}

ConnectionPool<PGconn*>*
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
    std::string connStr = buildPqConnStr(info);

    dbData->connectionPool = std::make_unique<ConnectionPool<PGconn*>>(
        poolSize,
        // factory
        [connStr]() -> PGconn* {
            PGconn* conn = PQconnectdb(connStr.c_str());
            if (PQstatus(conn) != CONNECTION_OK) {
                std::string err = PQerrorMessage(conn);
                PQfinish(conn);
                throw std::runtime_error("PostgreSQL connection failed: " + err);
            }
            return conn;
        },
        // closer
        [](PGconn* conn) { PQfinish(conn); },
        // validator
        [](PGconn* conn) { return PQstatus(conn) == CONNECTION_OK; });
}

ConnectionPool<PGconn*>::Session PostgresDatabase::getSession() const {
    std::lock_guard lock(sessionMutex);

    const std::string targetDb = connectionInfo.database;

    // Find connection pool in databaseDataCache
    auto it = databaseDataCache.find(targetDb);
    if (it == databaseDataCache.end() || !it->second || !it->second->connectionPool) {
        throw std::runtime_error(
            "PostgresDatabase::getSession: Connection pool not available for database: " +
            targetDb);
    }

    return it->second->connectionPool->acquire();
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
        const std::string sql =
            std::format("ALTER DATABASE \"{}\" RENAME TO \"{}\"", oldName, newName);

        auto session = getSession();
        PGconn* conn = session.get();
        PgResultPtr res(PQexec(conn, sql.c_str()));
        if (!res || PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
            std::string err = res ? PQresultErrorMessage(res.get()) : PQerrorMessage(conn);
            Logger::error(std::format("Failed to rename database: {}", err));
            return {false, err};
        }

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
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to rename database: {}", e.what()));
        return {false, e.what()};
    }
}

std::pair<bool, std::string> PostgresDatabase::createDatabase(const std::string& dbName,
                                                              const std::string& comment) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    if (dbName.empty()) {
        return {false, "Database name cannot be empty"};
    }

    try {
        auto session = getSession();
        PGconn* conn = session.get();

        const std::string sql = std::format("CREATE DATABASE \"{}\"", dbName);
        PgResultPtr createRes(PQexec(conn, sql.c_str()));
        if (!createRes || PQresultStatus(createRes.get()) != PGRES_COMMAND_OK) {
            std::string err =
                createRes ? PQresultErrorMessage(createRes.get()) : PQerrorMessage(conn);
            Logger::error(std::format("Failed to create database: {}", err));
            return {false, err};
        }

        if (!comment.empty()) {
            const std::string commentSql = std::format("COMMENT ON DATABASE \"{}\" IS '{}'", dbName,
                                                       escapeSingleQuotes(comment));
            PgResultPtr commentRes(PQexec(conn, commentSql.c_str()));
            if (!commentRes || PQresultStatus(commentRes.get()) != PGRES_COMMAND_OK) {
                std::string err =
                    commentRes ? PQresultErrorMessage(commentRes.get()) : PQerrorMessage(conn);
                Logger::warn(std::format("Database '{}' created, but failed to set comment: {}",
                                         dbName, err));
                return {true, std::format("Created database, but failed to set comment: {}", err)};
            }
        }

        Logger::info(std::format("Database '{}' created successfully", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to create database: {}", e.what()));
        return {false, e.what()};
    }
}

std::pair<bool, std::string> PostgresDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        // If dropping the currently connected database, we need a temporary
        // connection to the 'postgres' maintenance database since PostgreSQL
        // won't allow dropping a database with active connections.
        const std::string originalDb = connectionInfo.database;
        const bool isDroppingConnectedDb = (dbName == originalDb);

        // Helper lambda: terminate backends + drop, using a given connection
        auto execDrop = [&](PGconn* conn) -> std::pair<bool, std::string> {
            const std::string terminateSql =
                std::format("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                            "WHERE datname = '{}' AND pid <> pg_backend_pid()",
                            dbName);
            PgResultPtr termRes(PQexec(conn, terminateSql.c_str()));

            const std::string dropSql = std::format("DROP DATABASE \"{}\"", dbName);
            PgResultPtr dropRes(PQexec(conn, dropSql.c_str()));
            if (!dropRes || PQresultStatus(dropRes.get()) != PGRES_COMMAND_OK) {
                std::string err =
                    dropRes ? PQresultErrorMessage(dropRes.get()) : PQerrorMessage(conn);
                return {false, err};
            }
            return {true, ""};
        };

        std::pair<bool, std::string> dropResult;

        if (isDroppingConnectedDb) {
            // Connect to maintenance DB first so failure paths do not leave
            // the object with a destroyed active pool.
            auto tempInfo = connectionInfo;
            tempInfo.database = "postgres";
            std::string tempConnStr = buildPqConnStr(tempInfo);
            PGconn* tempConn = PQconnectdb(tempConnStr.c_str());
            if (PQstatus(tempConn) != CONNECTION_OK) {
                std::string err = PQerrorMessage(tempConn);
                PQfinish(tempConn);
                return {false, std::format("Failed to connect to maintenance database: {}", err)};
            }

            // Destroy the target DB pool so active sessions are closed before DROP.
            {
                std::lock_guard lock(sessionMutex);
                auto it = databaseDataCache.find(dbName);
                if (it != databaseDataCache.end() && it->second) {
                    it->second->connectionPool.reset();
                }
            }

            dropResult = execDrop(tempConn);
            PQfinish(tempConn);

            if (!dropResult.first) {
                // Best-effort recovery: recreate original active pool.
                try {
                    ensureConnectionPoolForDatabase(connectionInfo);
                } catch (const std::exception& restoreErr) {
                    Logger::warn(std::format("Failed to restore connection pool for '{}': {}",
                                             originalDb, restoreErr.what()));
                }
            }
        } else {
            auto session = getSession();
            dropResult = execDrop(session.get());
        }

        if (!dropResult.first) {
            Logger::error(std::format("Failed to drop database: {}", dropResult.second));
            return dropResult;
        }

        // Remove from cache
        databaseDataCache.erase(dbName);

        // If we dropped the connected database, switch to 'postgres'
        if (isDroppingConnectedDb) {
            connectionInfo.database = "postgres";
            try {
                ensureConnectionPoolForDatabase(connectionInfo);
                connected = true;
                setLastConnectionError("");
            } catch (const std::exception& switchErr) {
                connected = false;
                const std::string switchError = std::format(
                    "Database dropped, but failed to switch active connection to postgres: {}",
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
