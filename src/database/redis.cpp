#include "database/redis.hpp"

namespace {
    QueryResult notConnected() {
        QueryResult r;
        r.statements.push_back({.success = false, .errorMessage = "Not connected to Redis server"});
        return r;
    }
} // namespace

RedisDatabase::~RedisDatabase() {
    // join every worker while the members they touch still exist
    connectionOp.wait();
    refreshWorkflow_.wait();
    dbInfoLoadOp_.wait();
    RedisDatabase::disconnect();
}

std::pair<bool, std::string> RedisDatabase::connect() {
    if (connected)
        return {true, ""};
    setAttemptedConnection(true);
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }
    auto conn = std::make_shared<dearsql::RedisConnection>(connectionInfo);
    auto [ok, err] = conn->open();
    if (!ok) {
        spdlog::error("Redis connection failed: {}", err);
        stopSshTunnel();
        setLastConnectionError(err);
        return {false, err};
    }
    {
        std::lock_guard lock(connMutex_);
        conn_ = std::move(conn);
    }
    connected = true;
    setLastConnectionError("");
    return {true, ""};
}

void RedisDatabase::disconnect() {
    {
        std::lock_guard lock(connMutex_);
        if (conn_)
            conn_->close();
        conn_.reset();
    }
    stopSshTunnel();
    connected = false;
}

void RedisDatabase::refreshConnection() {
    refreshWorkflow_.start([this] {
        disconnect();
        setLastConnectionError("");
        return connect().first;
    });
}

void RedisDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow_.check([this](bool ok) {
        if (ok)
            startDbInfoLoadAsync(true);
    });
}

QueryResult RedisDatabase::executeQuery(const std::string& command, int rowLimit) {
    auto conn = connection();
    return conn ? conn->database()->execute(command, rowLimit) : notConnected();
}

QueryResult RedisDatabase::executeQueryInDatabase(int dbIndex, const std::string& command,
                                                  int rowLimit) {
    auto conn = connection();
    return conn ? conn->database(std::to_string(dbIndex))->execute(command, rowLimit)
                : notConnected();
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
RedisDatabase::getTableDataForDatabase(int dbIndex, const std::string& pattern, int limit,
                                       int offset) {
    auto conn = connection();
    if (!conn)
        return {};
    auto db = conn->database(std::to_string(dbIndex));
    Table keys;
    keys.name = pattern;
    return {db->getColumnNames(keys), db->getTableData(keys, limit, offset)};
}

int RedisDatabase::getSelectedDatabase() const {
    auto conn = connection();
    return conn ? conn->selectedDatabase() : 0;
}

void RedisDatabase::startDbInfoLoadAsync(bool forceRefresh) {
    if (dbInfoLoadOp_.isRunning() || (dbInfoLoaded_ && !forceRefresh))
        return;
    dbInfoLoadOp_.start([this] {
        std::string err;
        auto conn = connection();
        return conn ? libCall(err, "redis db info", [&] { return conn->databaseInfo(); })
                    : std::vector<RedisDbInfo>{};
    });
}

void RedisDatabase::checkDbInfoStatusAsync() {
    dbInfoLoadOp_.check([this](std::vector<RedisDbInfo> info) {
        dbInfoList_ = std::move(info);
        dbInfoLoaded_ = true;
    });
}
