#pragma once

#include "async_helper.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include <atomic>
#include <hiredis/hiredis.h>
#include <hiredis/hiredis_ssl.h>
#include <mutex>

struct RedisKey {
    std::string name;
    std::string type;
    std::string value;
    int64_t ttl = -1;
    int64_t size = -1;
};

struct RedisDbInfo {
    int index = 0;
    int64_t keys = 0;
    int64_t expires = 0;
    int64_t avgTtl = 0;
    bool hasKeys = false;
};

class RedisDatabase final : public DatabaseInterface, public IQueryExecutor {
public:
    RedisDatabase(const DatabaseConnectionInfo& connInfo);
    ~RedisDatabase() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;
    void checkRefreshWorkflowAsync();

    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow_.isRunning();
    }

    void checkTablesStatusAsync();

    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

    std::vector<std::vector<std::string>> getTableData(const std::string& keyPattern, int limit,
                                                       int offset);
    std::vector<std::string> getColumnNames(const std::string& keyPattern);
    int getRowCount(const std::string& keyPattern);

    std::vector<RedisKey> getKeys(const std::string& pattern = "*", int limit = 1000) const;
    std::string getKeyValue(const std::string& key, const std::string& knownType = "") const;
    std::string getKeyType(const std::string& key) const;
    int64_t getKeyTTL(const std::string& key) const;

    bool selectDatabase(int dbIndex);

    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    getTableDataForDatabase(int dbIndex, const std::string& pattern, int limit, int offset);
    QueryResult executeQueryInDatabase(int dbIndex, const std::string& query, int rowLimit = 1000);

    int getSelectedDatabase() const {
        return selectedDbIndex_;
    }
    int getDatabaseCount() const {
        return numDatabases_;
    }
    const std::vector<RedisDbInfo>& getDatabaseInfoList() const {
        return dbInfoList_;
    }
    void startDbInfoLoadAsync(bool forceRefresh = false);
    void checkDbInfoStatusAsync();
    bool isDbInfoLoaded() const {
        return dbInfoLoaded_;
    }
    bool isLoadingDbInfo() const {
        return loadingDbInfo_.load();
    }

    void startKeysLoadAsync(bool forceRefresh = false);
    void checkKeysStatusAsync();
    std::vector<Table> getKeysAsync();

    const std::vector<Table>& getKeyGroups() const {
        return tables;
    }

    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || loadingKeys.load() || loadingDbInfo_.load() ||
               refreshWorkflow_.isRunning();
    }

    std::atomic<bool> loadingKeys = false;
    bool keysLoaded = false;
    std::string lastKeysError;

protected:
    std::vector<std::string> getTableNames();

private:
    mutable std::mutex contextMutex_;
    redisContext* context = nullptr;
    redisSSLContext* sslCtx_ = nullptr;

    AsyncOperation<bool> refreshWorkflow_;

    AsyncOperation<std::vector<Table>> keysLoadOp_;
    std::vector<Table> tables;

    int selectedDbIndex_ = 0;
    int numDatabases_ = 1;
    std::vector<RedisDbInfo> dbInfoList_;
    std::atomic<bool> loadingDbInfo_ = false;
    bool dbInfoLoaded_ = false;
    AsyncOperation<std::vector<RedisDbInfo>> dbInfoLoadOp_;
    std::vector<RedisDbInfo> fetchDatabaseInfo();

    mutable std::mutex operationMutex_;

    redisReply* executeRedisCommandParsed(const std::vector<std::string>& commandParts) const;
    bool beginDatabaseScopedOperation(int dbIndex, int& previousDbIndex);
    void endDatabaseScopedOperation(int previousDbIndex, int activeDbIndex);
    static std::string formatRedisReply(redisReply* reply);
    static std::vector<std::string> parseRedisCommand(const std::string& command);
    void groupKeysByPattern(std::vector<Table>& out) const;
};
