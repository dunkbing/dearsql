#include "database/oracle.hpp"
#include "database/ddl_utils.hpp"
#include "database/oracle/oracle_client_installer.hpp"
#include "oracle/oracle_utils.hpp"
#include <cstdlib>
#include <format>
#include <spdlog/spdlog.h>

#if defined(__linux__)
#include <dlfcn.h>
#endif
#include <mutex>
#include <ranges>
#include <vector>

static std::mutex g_dpiCtxMutex;
static dpiContext* g_dpiCtx = nullptr;
static bool g_dpiCtxAttempted = false;
static std::string g_dpiCtxInitError;
static std::string g_oracleClientLibDir;

void OracleDatabase::initContext() {
    std::lock_guard lock(g_dpiCtxMutex);
    if (g_dpiCtx) {
        return;
    }
    if (g_dpiCtxAttempted && !OracleClientInstaller::isInstalled()) {
        return; // already failed and nothing changed
    }

    g_dpiCtxAttempted = true;
    g_dpiCtxInitError.clear();

    dpiContextCreateParams params{};

    // check for locally installed Oracle Client
    if (OracleClientInstaller::isInstalled()) {
        g_oracleClientLibDir = OracleClientInstaller::getInstallDir();
        params.oracleClientLibDir = g_oracleClientLibDir.c_str();
        spdlog::debug("Using Oracle Client from: {}", g_oracleClientLibDir);

#if defined(__linux__)
        // ensure libaio.so.1 with correct SONAME is bundled
        OracleClientInstaller::ensureLibaio(std::filesystem::path(g_oracleClientLibDir));

        // pre-load libaio with RTLD_GLOBAL before ODPI-C loads libclntsh.so.
        // this works because glibc checks already-loaded libraries by SONAME
        // when resolving DT_NEEDED deps. the Ubuntu 22.04 libaio has
        // SONAME "libaio.so.1" which matches what libclntsh.so expects.
        std::string libaioPath = g_oracleClientLibDir + "/libaio.so.1";
        void* h = dlopen(libaioPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            spdlog::debug("Pre-loaded {}", libaioPath);
        } else {
            spdlog::warn("Failed to pre-load libaio: {}", dlerror() ? dlerror() : "unknown error");
        }
#endif
    }

    dpiErrorInfo errorInfo;
    if (dpiContext_createWithParams(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &params, &g_dpiCtx,
                                    &errorInfo) != DPI_SUCCESS) {
        std::string msg(errorInfo.message, errorInfo.messageLength);
        spdlog::error("Failed to create ODPI-C context: {}", msg);
        if (msg.find("DPI-1047") != std::string::npos) {
            g_dpiCtxInitError = "Oracle Instant Client is not installed.";
        } else {
            g_dpiCtxInitError = msg;
        }
    }
}

dpiContext* OracleDatabase::getContext() {
    initContext();
    return g_dpiCtx;
}

bool OracleDatabase::needsClientInstall() {
    initContext();
    return g_dpiCtx == nullptr && !g_dpiCtxInitError.empty();
}

void OracleDatabase::reinitContext() {
    std::lock_guard lock(g_dpiCtxMutex);
    g_dpiCtxAttempted = false;
    g_dpiCtxInitError.clear();
}

OracleDatabase::OracleDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    if (connectionInfo.sslmode == SslMode::Prefer || connectionInfo.sslmode == SslMode::Allow) {
        connectionInfo.sslmode = SslMode::Disable;
    }
    spdlog::debug("Creating OracleDatabase with service = '{}', showAllDatabases = {}",
                  connectionInfo.database, connInfo.showAllDatabases);
    // service name can be empty — connect() will auto-detect
}

OracleDatabase::~OracleDatabase() {
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

OracleDatabaseNode* OracleDatabase::getDatabaseData(const std::string& schemaName) {
    const auto it = databaseDataCache.find(schemaName);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<OracleDatabaseNode>();
        newData->name = schemaName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[schemaName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> OracleDatabase::connect() {
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);
    initContext();

    if (!g_dpiCtx) {
        std::string error =
            g_dpiCtxInitError.empty()
                ? "Oracle Instant Client is not installed. "
                  "Use the Install button in the sidebar after adding this connection."
                : g_dpiCtxInitError;
        connected = false;
        setLastConnectionError(error);
        return {false, error};
    }

    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        connected = false;
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }

    // if no service name specified, probe common ones with a single connection
    if (connectionInfo.database.empty()) {
        std::string lastError;
        for (const auto& candidate : {"FREEPDB1", "XEPDB1", "XE", "ORCL", "FREE"}) {
            connectionInfo.database = candidate;
            try {
                dpiConn* probe = openDpiConnection(connectionInfo);
                closeDpiConnection(probe);
                spdlog::debug("Auto-detected Oracle service: {}", candidate);
                break;
            } catch (const std::exception& e) {
                lastError = e.what();
                spdlog::debug("Oracle service '{}' not available", candidate);
                connectionInfo.database.clear();
            }
        }
        if (connectionInfo.database.empty()) {
            connected = false;
            setLastConnectionError(
                "Could not find a valid Oracle service. Try specifying the service name.");
            return {false, getLastConnectionError()};
        }
    }

    try {
        ensureConnectionPoolForSchema(connectionInfo);
        spdlog::debug("Successfully connected to Oracle: {}", connectionInfo.database);
        connected = true;
        setLastConnectionError("");

        if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning()) {
            spdlog::debug("Starting async schema loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Connection to Oracle failed: {}", e.what());
        std::lock_guard lock(sessionMutex);
        std::string upperUser = ddl_utils::toUpper(connectionInfo.username);
        auto it = databaseDataCache.find(upperUser);
        if (it != databaseDataCache.end() && it->second) {
            it->second->connectionPool.reset();
        }
        connected = false;
        std::string error = "Oracle connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void OracleDatabase::disconnect() {
    if (AsyncOperationControl::skipWaitOnDestroy().load(std::memory_order_relaxed)) {
        std::unique_lock lock(sessionMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            spdlog::warn("OracleDatabase::disconnect: skipping pool teardown during shutdown");
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

void OracleDatabase::refreshConnection() {
    std::string upperUser = ddl_utils::toUpper(connectionInfo.username);
    getDatabaseData(upperUser);

    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        try {
            ensureConnectionPoolForSchema(connectionInfo);
            spdlog::debug("Successfully reconnected to Oracle: {}", connectionInfo.database);
            connected = true;
            setLastConnectionError("");
        } catch (const std::exception& e) {
            spdlog::error("Oracle reconnection failed: {}", e.what());
            setLastConnectionError(e.what());
            return false;
        }

        if (connectionInfo.showAllDatabases) {
            spdlog::debug("Loading schema names synchronously for refresh...");
            auto schemas = getDatabaseNamesAsync();

            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(schemas);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }

        spdlog::debug("Oracle refresh workflow completed for {} schemas", databaseDataCache.size());
        return true;
    });
}

QueryResult OracleDatabase::executeQuery(const std::string& query, int rowLimit) {
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
        result = dpiExecuteQuery(session.get(), query, rowLimit);
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

std::unordered_map<std::string, std::unique_ptr<OracleDatabaseNode>>&
OracleDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void OracleDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }

    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool OracleDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool OracleDatabase::hasPendingAsyncWork() const {
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

void OracleDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& schemas) {
        spdlog::debug("Async schema loading completed. Found {} schemas", schemas.size());

        for (const auto& schemaName : schemas) {
            getDatabaseData(schemaName);
        }

        databasesLoaded = true;
    });
}

void OracleDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (success) {
            spdlog::debug("Oracle refresh workflow completed successfully");
            std::vector<std::string> refreshedSchemas;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshedSchemas = std::move(pendingRefreshDatabaseNames);
                pendingRefreshDatabaseNames.clear();
            }

            for (const auto& schemaName : refreshedSchemas) {
                getDatabaseData(schemaName);
            }

            databasesLoaded = true;

            for (auto& [_, dbDataPtr] : databaseDataCache) {
                if (dbDataPtr) {
                    dbDataPtr->startTablesLoadAsync(true);
                    dbDataPtr->startViewsLoadAsync(true);
                }
            }
        } else {
            spdlog::error("Oracle refresh workflow failed");
        }
    });
}

std::vector<std::string> OracleDatabase::getDatabaseNamesAsync() const {
    spdlog::debug("getDatabaseNamesAsync (Oracle)");
    std::vector<std::string> result;

    try {
        if (!isConnected()) {
            spdlog::error("Cannot load schemas: not connected");
            return result;
        }

        if (!connectionInfo.showAllDatabases) {
            std::string upperUser = ddl_utils::toUpper(connectionInfo.username);
            result.push_back(upperUser);
            return result;
        }

        auto session = getSession();
        dpiConn* conn = session.get();

        result = dpiQueryStringList(
            conn,
            "SELECT DISTINCT OWNER FROM ALL_OBJECTS "
            "WHERE OBJECT_TYPE IN ('TABLE','VIEW','SEQUENCE','PROCEDURE','FUNCTION','PACKAGE') "
            "AND OWNER NOT IN ('SYS','SYSTEM','DBSNMP','OUTLN','MDSYS','ORDSYS','ORDDATA',"
            "'CTXSYS','XDB','WMSYS','APPQOSSYS','DBSFWUSER','REMOTE_SCHEDULER_AGENT',"
            "'GSMADMIN_INTERNAL','OJVMSYS','LBACSYS','GGSYS') "
            "ORDER BY OWNER");
    } catch (const std::exception& e) {
        spdlog::error("Failed to execute async schema query: {}", e.what());
    }

    spdlog::debug("Async query completed. Found {} schemas", result.size());
    return result;
}

void OracleDatabase::ensureConnectionPoolForSchema(const DatabaseConnectionInfo& info) {
    std::string schemaName = ddl_utils::toUpper(info.username);

    {
        std::lock_guard lock(sessionMutex);
        auto* schemaData = getDatabaseData(schemaName);
        if (!schemaData || schemaData->connectionPool) {
            return;
        }
    }

    auto newPool = std::make_unique<ConnectionPool<dpiConn*>>(
        [info]() -> dpiConn* { return openDpiConnection(info); },
        [](dpiConn* conn) { closeDpiConnection(conn); },
        [](dpiConn* conn) -> bool { return dpiConnectionAlive(conn); });

    std::lock_guard lock(sessionMutex);
    auto* schemaData = getDatabaseData(schemaName);
    if (!schemaData || schemaData->connectionPool) {
        return;
    }
    schemaData->connectionPool = std::move(newPool);
}

ConnectionPool<dpiConn*>::Session OracleDatabase::getSession() const {
    std::lock_guard lock(sessionMutex);

    std::string schemaName = ddl_utils::toUpper(connectionInfo.username);

    auto it = databaseDataCache.find(schemaName);
    if (it == databaseDataCache.end() || !it->second || !it->second->connectionPool) {
        throw std::runtime_error(
            "OracleDatabase::getSession: Connection pool not available for schema: " + schemaName);
    }

    return it->second->connectionPool->acquire();
}

std::pair<bool, std::string> OracleDatabase::createDatabase(const std::string& dbName,
                                                            const std::string& comment) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    if (dbName.empty()) {
        return {false, "Schema name cannot be empty"};
    }

    try {
        auto session = getSession();
        dpiConn* conn = session.get();

        // Oracle "create database" = create user/schema
        std::string sql = std::format("CREATE USER \"{}\" IDENTIFIED BY \"{}\"", dbName, dbName);
        auto result = dpiExecuteQuery(conn, sql, 0);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }

        // grant basic privileges
        dpiExecuteQuery(conn, std::format("GRANT CONNECT, RESOURCE TO \"{}\"", dbName), 0);

        spdlog::debug("Schema '{}' created successfully", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Failed to create schema: {}", e.what());
        return {false, e.what()};
    }
}

std::pair<bool, std::string> OracleDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        auto session = getSession();
        dpiConn* conn = session.get();

        // Oracle "drop database" = drop user cascade
        std::string sql = std::format("DROP USER \"{}\" CASCADE", dbName);
        auto result = dpiExecuteQuery(conn, sql, 0);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }

        databaseDataCache.erase(dbName);
        spdlog::debug("Schema '{}' dropped successfully", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("Failed to drop schema: {}", e.what());
        return {false, e.what()};
    }
}
