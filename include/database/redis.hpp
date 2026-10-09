#pragma once

#include "async_helper.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include <dearsql/backends/redis_connection.hpp>
#include <mutex>

using dearsql::RedisDbInfo;
using dearsql::RedisKey;

// redis server over libdearsql; logical dbs are addressed by index. adds the ssh
// tunnel, async connect/refresh and the per-db stats the sidebar shows
class RedisDatabase final : public DatabaseInterface, public IQueryExecutor {
public:
    explicit RedisDatabase(const DatabaseConnectionInfo& info) {
        connectionInfo = info;
    }
    ~RedisDatabase() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;
    void checkRefreshWorkflowAsync();
    [[nodiscard]] bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow_.isRunning();
    }

    // commands run in the selected db
    QueryResult executeQuery(const std::string& command, int rowLimit = 1000) override;
    QueryResult executeQueryInDatabase(int dbIndex, const std::string& command,
                                       int rowLimit = 1000);
    // Key/Type/Value/TTL/Size rows for keys matching pattern in one db
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    getTableDataForDatabase(int dbIndex, const std::string& pattern, int limit, int offset);

    [[nodiscard]] int getSelectedDatabase() const;
    const std::vector<RedisDbInfo>& getDatabaseInfoList() const {
        return dbInfoList_;
    }
    void startDbInfoLoadAsync(bool forceRefresh = false);
    void checkDbInfoStatusAsync();
    [[nodiscard]] bool isDbInfoLoaded() const {
        return dbInfoLoaded_;
    }
    [[nodiscard]] bool isLoadingDbInfo() const {
        return dbInfoLoadOp_.isRunning();
    }

    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || dbInfoLoadOp_.isRunning();
    }

    // the library connection (null when disconnected)
    [[nodiscard]] std::shared_ptr<dearsql::RedisConnection> connection() const {
        std::lock_guard lock(connMutex_);
        return conn_;
    }
    [[nodiscard]] std::shared_ptr<dearsql::IConnection> libConnection() const override {
        return connection();
    }

private:
    std::shared_ptr<dearsql::RedisConnection> conn_;
    mutable std::mutex connMutex_;
    AsyncOperation<bool> refreshWorkflow_;
    AsyncOperation<std::vector<RedisDbInfo>> dbInfoLoadOp_;
    std::vector<RedisDbInfo> dbInfoList_;
    bool dbInfoLoaded_ = false;
};
