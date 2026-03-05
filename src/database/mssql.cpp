#include "database/mssql.hpp"
#include "mssql/mssql_utils.hpp"
#include "utils/logger.hpp"
#include <format>
#include <mutex>
#include <ranges>
#include <vector>

// thread-local error string captured by db-lib callbacks
thread_local std::string tls_lastError;

namespace {

    int dblibErrorHandler(DBPROCESS* /*dbproc*/, int /*severity*/, int /*dberr*/, int /*oserr*/,
                          char* dberrstr, char* oserrstr) {
        std::string msg;
        if (dberrstr)
            msg = dberrstr;
        if (oserrstr && oserrstr[0]) {
            if (!msg.empty())
                msg += "; ";
            msg += oserrstr;
        }
        tls_lastError = msg;
        return INT_CANCEL;
    }

    int dblibMessageHandler(DBPROCESS* /*dbproc*/, DBINT /*msgno*/, int /*msgstate*/, int severity,
                            char* msgtext, char* /*srvname*/, char* /*procname*/, int /*line*/) {
        if (severity > 10 && msgtext) {
            tls_lastError = msgtext;
        }
        return 0;
    }

} // namespace

static std::once_flag g_dbLibInitFlag;

void MSSQLDatabase::initDbLib() {
    std::call_once(g_dbLibInitFlag, []() {
        dbinit();
        dberrhandle(dblibErrorHandler);
        dbmsghandle(dblibMessageHandler);
    });
}

MSSQLDatabase::MSSQLDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    Logger::debug(std::format("Creating MSSQLDatabase with database = '{}', showAllDatabases = {}",
                              connectionInfo.database, connInfo.showAllDatabases));
    if (connectionInfo.database.empty()) {
        connectionInfo.database = "master";
    }
    initDbLib();
}

MSSQLDatabase::~MSSQLDatabase() {
    databasesLoader.cancel();
    refreshWorkflow.cancel();

    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->tablesLoader.cancel();
            dbDataPtr->viewsLoader.cancel();
        }
    }

    disconnect();
}

MSSQLDatabaseNode* MSSQLDatabase::getDatabaseData(const std::string& dbName) {
    const auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<MSSQLDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> MSSQLDatabase::connect() {
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
        Logger::info("Successfully connected to MSSQL database: " + connectionInfo.database);
        connected = true;
        setLastConnectionError("");

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
        std::string error = "MSSQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MSSQLDatabase::disconnect() {
    if (AsyncOperationControl::skipWaitOnDestroy().load(std::memory_order_relaxed)) {
        std::unique_lock lock(sessionMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            Logger::warn("MSSQLDatabase::disconnect: skipping pool teardown during shutdown");
            connected = false;
            return;
        }

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
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->connectionPool.reset();
        }
    }
    stopSshTunnel();
    connected = false;
}

void MSSQLDatabase::refreshConnection() {
    getDatabaseData(connectionInfo.database);

    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        try {
            ensureConnectionPoolForDatabase(connectionInfo);
            Logger::info("Successfully reconnected to MSSQL database: " + connectionInfo.database);
            connected = true;
            setLastConnectionError("");
        } catch (const std::exception& e) {
            Logger::error("MSSQL reconnection failed: " + std::string(e.what()));
            setLastConnectionError(e.what());
            return false;
        }

        if (connectionInfo.showAllDatabases) {
            Logger::debug("Loading database names synchronously for refresh...");
            auto databases = getDatabaseNamesAsync();

            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(databases);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }

        Logger::info(std::format("MSSQL refresh workflow completed for {} databases",
                                 databaseDataCache.size()));
        return true;
    });
}

QueryResult MSSQLDatabase::executeQuery(const std::string& query, int rowLimit) {
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
        DBPROCESS* dbproc = session.get();

        clearLastError();
        dbcmd(dbproc, query.c_str());

        if (dbsqlexec(dbproc) == FAIL) {
            StatementResult r;
            r.success = false;
            r.errorMessage = getLastError();
            result.statements.push_back(r);

            dbcancel(dbproc);

            const auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTimeMs =
                std::chrono::duration<double, std::milli>(endTime - startTime).count();
            return result;
        }

        // process all result sets (multi-statement batches)
        RETCODE rc;
        while ((rc = dbresults(dbproc)) != NO_MORE_RESULTS) {
            if (rc == FAIL) {
                StatementResult r;
                r.success = false;
                r.errorMessage = getLastError();
                result.statements.push_back(r);
                break;
            }
            auto r = extractDbLibResult(dbproc, rowLimit);
            if (r.success || !r.errorMessage.empty()) {
                result.statements.push_back(std::move(r));
            }
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

std::unordered_map<std::string, std::unique_ptr<MSSQLDatabaseNode>>&
MSSQLDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void MSSQLDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }

    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool MSSQLDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool MSSQLDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases()) {
        return true;
    }

    for (const auto& [_, dbNode] : databaseDataCache) {
        if (!dbNode)
            continue;
        if (dbNode->tablesLoader.isRunning() || dbNode->viewsLoader.isRunning()) {
            return true;
        }
    }
    return false;
}

void MSSQLDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        Logger::info(
            std::format("Async database loading completed. Found {} databases", databases.size()));

        for (const auto& dbName : databases) {
            getDatabaseData(dbName);
        }

        databasesLoaded = true;
    });
}

void MSSQLDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (success) {
            Logger::info("MSSQL refresh workflow completed successfully");
            std::vector<std::string> refreshedDatabases;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshedDatabases = std::move(pendingRefreshDatabaseNames);
                pendingRefreshDatabaseNames.clear();
            }

            for (const auto& dbName : refreshedDatabases) {
                getDatabaseData(dbName);
            }

            databasesLoaded = true;

            for (auto& [_, dbDataPtr] : databaseDataCache) {
                if (dbDataPtr) {
                    dbDataPtr->startTablesLoadAsync(true);
                    dbDataPtr->startViewsLoadAsync(true);
                }
            }
        } else {
            Logger::error("MSSQL refresh workflow failed");
        }
    });
}

std::vector<std::string> MSSQLDatabase::getDatabaseNamesAsync() const {
    Logger::info("getDatabaseNamesAsync (MSSQL)");
    std::vector<std::string> result;

    try {
        if (!isConnected()) {
            Logger::error("Cannot load databases: not connected");
            return result;
        }

        if (!connectionInfo.showAllDatabases) {
            result.push_back(connectionInfo.database);
            return result;
        }

        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        if (execQuery(dbproc,
                      "SELECT name FROM sys.databases WHERE database_id > 4 ORDER BY name") &&
            dbresults(dbproc) == SUCCEED) {
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                std::string val = colToString(dbproc, 1);
                if (val != "NULL") {
                    result.push_back(val);
                }
            }
        }
        drainResults(dbproc);
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to execute async database query: {}", e.what()));
    }

    Logger::info(std::format("Async query completed. Found {} databases", result.size()));
    return result;
}

void MSSQLDatabase::ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info) {
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
    auto newPool = std::make_unique<ConnectionPool<DBPROCESS*>>(
        poolSize, [info]() -> DBPROCESS* { return openDbLibConnection(info, info.database); },
        [](DBPROCESS* dbproc) { dbclose(dbproc); },
        [](DBPROCESS* dbproc) -> bool { return !dbdead(dbproc); });

    std::lock_guard lock(sessionMutex);
    auto* dbData = getDatabaseData(info.database);
    if (!dbData || dbData->connectionPool) {
        return;
    }
    dbData->connectionPool = std::move(newPool);
}

ConnectionPool<DBPROCESS*>::Session MSSQLDatabase::getSession() const {
    std::lock_guard lock(sessionMutex);

    const std::string targetDb = connectionInfo.database;
    auto it = databaseDataCache.find(targetDb);
    if (it == databaseDataCache.end() || !it->second || !it->second->connectionPool) {
        throw std::runtime_error(
            "MSSQLDatabase::getSession: Connection pool not available for database: " + targetDb);
    }

    return it->second->connectionPool->acquire();
}

std::pair<bool, std::string> MSSQLDatabase::createDatabase(const std::string& dbName,
                                                           const std::string& comment) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    if (dbName.empty()) {
        return {false, "Database name cannot be empty"};
    }

    try {
        std::string sql = std::format("CREATE DATABASE [{}]", dbName);

        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        clearLastError();
        dbcmd(dbproc, sql.c_str());

        if (dbsqlexec(dbproc) == FAIL) {
            std::string err = getLastError();
            Logger::error(std::format("Failed to create database: {}", err));
            return {false, err};
        }

        drainResults(dbproc);

        Logger::info(std::format("Database '{}' created successfully", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to create database: {}", e.what()));
        return {false, e.what()};
    }
}

std::pair<bool, std::string> MSSQLDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        const std::string originalDb = connectionInfo.database;
        const bool isDroppingConnectedDb = (dbName == originalDb);

        // open a temporary connection to 'master' to execute the drop
        DbProcessGuard tempProc(openDbLibConnection(connectionInfo, "master"));

        // destroy the connection pool for the target database
        if (isDroppingConnectedDb) {
            std::lock_guard lock(sessionMutex);
            auto it = databaseDataCache.find(dbName);
            if (it != databaseDataCache.end() && it->second) {
                it->second->connectionPool.reset();
            }
        }

        std::string sql = std::format(
            "ALTER DATABASE [{}] SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE [{}]",
            dbName, dbName);

        clearLastError();
        dbcmd(tempProc.get(), sql.c_str());

        bool success = (dbsqlexec(tempProc.get()) != FAIL);
        std::string err;
        if (!success) {
            err = getLastError();
        }

        drainResults(tempProc.get());

        if (!success) {
            if (isDroppingConnectedDb) {
                try {
                    ensureConnectionPoolForDatabase(connectionInfo);
                } catch (...) {
                }
            }
            Logger::error(std::format("Failed to drop database: {}", err));
            return {false, err};
        }

        databaseDataCache.erase(dbName);

        if (isDroppingConnectedDb) {
            connectionInfo.database = "master";
            try {
                ensureConnectionPoolForDatabase(connectionInfo);
                connected = true;
                setLastConnectionError("");
            } catch (const std::exception& switchErr) {
                connected = false;
                const std::string switchError = std::format(
                    "Database dropped, but failed to switch to master: {}", switchErr.what());
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
