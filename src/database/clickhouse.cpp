#include "database/clickhouse.hpp"
#include "database/ddl_utils.hpp"
#include "database/sql_builder.hpp"
#include <chrono>
#include <clickhouse/client.h>
#include <format>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {

    clickhouse::ClientOptions makeClientOptions(const DatabaseConnectionInfo& info) {
        clickhouse::ClientOptions opts;
        opts.SetHost(info.host)
            .SetPort(static_cast<uint16_t>(info.port))
            .SetDefaultDatabase(info.database)
            .SetUser(info.username)
            .SetPassword(info.password)
            .SetRethrowException(true)
            .SetConnectionConnectTimeout(std::chrono::seconds(5));

        if (info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
            info.sslmode == SslMode::VerifyFull) {
            clickhouse::ClientOptions::SSLOptions ssl;
            if (info.sslmode == SslMode::Require)
                ssl.SetSkipVerification(true);
            if (!info.sslCACertPath.empty()) {
                ssl.SetPathToCAFiles({info.sslCACertPath});
                ssl.SetSkipVerification(false);
            }
            opts.SetSSLOptions(std::move(ssl));
        }

        return opts;
    }

    std::function<CHClientHandle()> makeClickHouseFactory(const DatabaseConnectionInfo& info) {
        return [info]() -> CHClientHandle {
            auto opts = makeClientOptions(info);
            auto* client = new clickhouse::Client(opts);
            client->Ping();
            return client;
        };
    }

    // convert a column value to string using toString() cast in CH
    // This avoids using the problematic Column API entirely
    std::string columnValueToString(const clickhouse::Block& block, size_t col, size_t row) {
        // Use Block Iterator to read column name+type, but for value extraction
        // we rely on the raw string representation via the column's own ToString.
        // Since the clickhouse-cpp Column API is broken with GCC 13, we use a simpler approach.
        auto column = block[col];
        if (!column || row >= column->Size())
            return "NULL";

        // For all types, use toString() from the native protocol data
        // The clickhouse native protocol sends typed data; we'll read string representation
        try {
            auto item = column->GetItem(row);
            auto sv = item.data;

            switch (column->Type()->GetCode()) {
            case clickhouse::Type::String:
            case clickhouse::Type::FixedString:
                return std::string(sv);
            case clickhouse::Type::UInt8:
                return std::to_string(*reinterpret_cast<const uint8_t*>(sv.data()));
            case clickhouse::Type::UInt16:
                return std::to_string(*reinterpret_cast<const uint16_t*>(sv.data()));
            case clickhouse::Type::UInt32:
                return std::to_string(*reinterpret_cast<const uint32_t*>(sv.data()));
            case clickhouse::Type::UInt64:
                return std::to_string(*reinterpret_cast<const uint64_t*>(sv.data()));
            case clickhouse::Type::Int8:
                return std::to_string(*reinterpret_cast<const int8_t*>(sv.data()));
            case clickhouse::Type::Int16:
                return std::to_string(*reinterpret_cast<const int16_t*>(sv.data()));
            case clickhouse::Type::Int32:
                return std::to_string(*reinterpret_cast<const int32_t*>(sv.data()));
            case clickhouse::Type::Int64:
                return std::to_string(*reinterpret_cast<const int64_t*>(sv.data()));
            case clickhouse::Type::Float32:
                return std::to_string(*reinterpret_cast<const float*>(sv.data()));
            case clickhouse::Type::Float64:
                return std::to_string(*reinterpret_cast<const double*>(sv.data()));
            default:
                if (!sv.empty() && sv.size() <= 256)
                    return std::string(sv);
                return "(binary)";
            }
        } catch (...) {
            return "(error)";
        }
    }

    StatementResult extractClickHouseResult(clickhouse::Client* client, const std::string& query,
                                            int rowLimit) {
        StatementResult result;
        int totalRows = 0;

        client->Select(query, [&](const clickhouse::Block& block) {
            if (block.GetRowCount() == 0)
                return;

            if (result.columnNames.empty()) {
                for (size_t c = 0; c < block.GetColumnCount(); ++c)
                    result.columnNames.push_back(block.GetColumnName(c));
            }

            for (size_t r = 0; r < block.GetRowCount() && totalRows < rowLimit; ++r) {
                std::vector<std::string> rowData;
                rowData.reserve(block.GetColumnCount());
                for (size_t c = 0; c < block.GetColumnCount(); ++c)
                    rowData.push_back(columnValueToString(block, c, r));
                result.tableData.push_back(std::move(rowData));
                ++totalRows;
            }
        });

        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        if (totalRows >= rowLimit)
            result.message += std::format(" (limited to {})", rowLimit);

        return result;
    }

} // namespace

ClickHouseDatabase::ClickHouseDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    if (connectionInfo.database.empty())
        connectionInfo.database = "default";
}

ClickHouseDatabase::~ClickHouseDatabase() {
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

ClickHouseDatabaseNode* ClickHouseDatabase::getDatabaseData(const std::string& dbName) {
    const auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<ClickHouseDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        newData->ensureConnectionPool();
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> ClickHouseDatabase::connect() {
    if (connected)
        return {true, ""};

    setAttemptedConnection(true);
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        connected = false;
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }

    try {
        ensureConnectionPoolForDatabase(connectionInfo);
        spdlog::debug("connected to ClickHouse: {}", connectionInfo.database);
        connected = true;
        setLastConnectionError("");

        if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning())
            refreshDatabaseNames();

        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("ClickHouse connection failed: {}", e.what());
        std::lock_guard lock(sessionMutex);
        auto it = databaseDataCache.find(connectionInfo.database);
        if (it != databaseDataCache.end() && it->second)
            it->second->connectionPool.reset();
        connected = false;
        std::string error = "ClickHouse connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void ClickHouseDatabase::disconnect() {
    if (AsyncOperationControl::skipWaitOnDestroy().load(std::memory_order_relaxed)) {
        std::unique_lock lock(sessionMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            connected = false;
            return;
        }
        for (auto& dbDataPtr : databaseDataCache | std::views::values) {
            if (dbDataPtr)
                dbDataPtr->connectionPool.reset();
        }
        stopSshTunnel();
        connected = false;
        return;
    }

    std::lock_guard lock(sessionMutex);
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr)
            dbDataPtr->connectionPool.reset();
    }
    stopSshTunnel();
    connected = false;
}

void ClickHouseDatabase::refreshConnection() {
    getDatabaseData(connectionInfo.database);

    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        try {
            ensureConnectionPoolForDatabase(connectionInfo);
            connected = true;
            setLastConnectionError("");
        } catch (const std::exception& e) {
            spdlog::error("ClickHouse reconnection failed: {}", e.what());
            setLastConnectionError(e.what());
            return false;
        }

        if (connectionInfo.showAllDatabases) {
            auto databases = getDatabaseNamesAsync();
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames = std::move(databases);
        } else {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatabaseNames.clear();
        }

        return true;
    });
}

QueryResult ClickHouseDatabase::executeQuery(const std::string& query, int rowLimit) {
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
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::string trimmed = query;
        auto pos = trimmed.find_first_not_of(" \t\n\r");
        if (pos != std::string::npos)
            trimmed = trimmed.substr(pos);

        bool isSelect = trimmed.size() >= 4 && (strncasecmp(trimmed.c_str(), "select", 6) == 0 ||
                                                strncasecmp(trimmed.c_str(), "show", 4) == 0 ||
                                                strncasecmp(trimmed.c_str(), "describe", 8) == 0 ||
                                                strncasecmp(trimmed.c_str(), "explain", 7) == 0 ||
                                                strncasecmp(trimmed.c_str(), "with", 4) == 0);

        if (isSelect) {
            auto r = extractClickHouseResult(client, query, rowLimit);
            result.statements.push_back(std::move(r));
        } else {
            clickhouse::Query q(query);
            client->Execute(q);
            StatementResult r;
            r.message = "Query executed successfully";
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

std::unordered_map<std::string, std::unique_ptr<ClickHouseDatabaseNode>>&
ClickHouseDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected())
        refreshDatabaseNames();
    return databaseDataCache;
}

void ClickHouseDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning())
        return;
    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool ClickHouseDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool ClickHouseDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases())
        return true;
    for (const auto& [_, dbNode] : databaseDataCache) {
        if (!dbNode)
            continue;
        if (dbNode->tablesLoader.isRunning() || dbNode->viewsLoader.isRunning())
            return true;
    }
    return false;
}

void ClickHouseDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        spdlog::debug("ClickHouse database loading completed. Found {} databases",
                      databases.size());
        for (const auto& dbName : databases)
            getDatabaseData(dbName);
        databasesLoaded = true;
    });
}

void ClickHouseDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (success) {
            std::vector<std::string> refreshedDatabases;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshedDatabases = std::move(pendingRefreshDatabaseNames);
                pendingRefreshDatabaseNames.clear();
            }
            for (const auto& dbName : refreshedDatabases)
                getDatabaseData(dbName);
            for (auto& dbDataPtr : databaseDataCache | std::views::values) {
                if (dbDataPtr)
                    dbDataPtr->ensureConnectionPool();
            }
            databasesLoaded = true;
            for (auto& [_, dbDataPtr] : databaseDataCache) {
                if (dbDataPtr) {
                    dbDataPtr->startTablesLoadAsync(true);
                    dbDataPtr->startViewsLoadAsync(true);
                }
            }
        } else {
            spdlog::error("ClickHouse refresh workflow failed");
        }
    });
}

std::vector<std::string> ClickHouseDatabase::getDatabaseNamesAsync() const {
    std::vector<std::string> result;

    try {
        if (!isConnected())
            return result;

        if (!connectionInfo.showAllDatabases) {
            result.push_back(connectionInfo.database);
            return result;
        }

        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        client->Select("SHOW DATABASES", [&](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r) {
                std::string dbName = columnValueToString(block, 0, r);
                if (dbName != "system" && dbName != "information_schema" &&
                    dbName != "INFORMATION_SCHEMA")
                    result.push_back(dbName);
            }
        });
    } catch (const std::exception& e) {
        spdlog::error("failed to query ClickHouse databases: {}", e.what());
    }

    return result;
}

void ClickHouseDatabase::ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info) {
    if (info.database.empty())
        throw std::runtime_error("ensureConnectionPoolForDatabase: database name is required");

    {
        std::lock_guard lock(sessionMutex);
        auto* dbData = getDatabaseData(info.database);
        if (!dbData || dbData->connectionPool)
            return;
    }

    auto newPool = std::make_unique<ConnectionPool<CHClientHandle>>(
        makeClickHouseFactory(info),
        [](CHClientHandle h) { delete static_cast<clickhouse::Client*>(h); },
        [](CHClientHandle h) -> bool {
            try {
                static_cast<clickhouse::Client*>(h)->Ping();
                return true;
            } catch (...) {
                return false;
            }
        });

    std::lock_guard lock(sessionMutex);
    auto* dbData = getDatabaseData(info.database);
    if (!dbData || dbData->connectionPool)
        return;
    dbData->connectionPool = std::move(newPool);
}

ConnectionPool<CHClientHandle>::Session ClickHouseDatabase::getSession() const {
    std::lock_guard lock(sessionMutex);
    const std::string targetDb = connectionInfo.database;

    auto it = databaseDataCache.find(targetDb);
    if (it == databaseDataCache.end() || !it->second || !it->second->connectionPool)
        throw std::runtime_error(
            "ClickHouseDatabase::getSession: Connection pool not available for database: " +
            targetDb);

    return it->second->connectionPool->acquire();
}

std::pair<bool, std::string> ClickHouseDatabase::renameDatabase(const std::string& /*oldName*/,
                                                                const std::string& /*newName*/) {
    return {false, "ClickHouse does not support renaming databases"};
}

std::pair<bool, std::string> ClickHouseDatabase::createDatabase(const std::string& dbName,
                                                                const std::string& comment) {
    if (!isConnected())
        return {false, "Not connected"};
    if (dbName.empty())
        return {false, "Database name cannot be empty"};

    try {
        const auto builder = createSQLBuilder(DatabaseType::CLICKHOUSE);
        std::string sql = std::format("CREATE DATABASE {}", builder->quoteIdentifier(dbName));
        if (!comment.empty())
            sql += std::format(" COMMENT '{}'", ddl_utils::escapeSingleQuotes(comment));

        auto session = getSession();
        clickhouse::Query q(sql);
        static_cast<clickhouse::Client*>(session.get())->Execute(q);

        spdlog::debug("database '{}' created", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("failed to create database: {}", e.what());
        return {false, e.what()};
    }
}

std::pair<bool, std::string>
ClickHouseDatabase::createDatabaseWithOptions(const CreateDatabaseOptions& opts) {
    return createDatabase(opts.name, opts.comment);
}

std::pair<bool, std::string> ClickHouseDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected())
        return {false, "Not connected"};

    try {
        const auto builder = createSQLBuilder(DatabaseType::CLICKHOUSE);
        std::string sql = std::format("DROP DATABASE {}", builder->quoteIdentifier(dbName));

        auto session = getSession();
        clickhouse::Query q(sql);
        static_cast<clickhouse::Client*>(session.get())->Execute(q);

        databaseDataCache.erase(dbName);
        spdlog::debug("database '{}' dropped", dbName);
        return {true, ""};
    } catch (const std::exception& e) {
        spdlog::error("failed to drop database: {}", e.what());
        return {false, e.what()};
    }
}
