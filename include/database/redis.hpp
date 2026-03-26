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
    int64_t ttl = -1;  // -1 means no expiration
    int64_t size = -1; // bytes, -1 if unavailable
};

// Information about a single Redis database (db0, db1, ..., db15)
struct RedisDbInfo {
    int index = 0;        // database index (0-15)
    int64_t keys = 0;     // number of keys
    int64_t expires = 0;  // number of keys with expiry
    int64_t avgTtl = 0;   // average TTL in milliseconds
    bool hasKeys = false; // whether this db has any keys
};

class RedisDatabase final : public DatabaseInterface, public IQueryExecutor {
public:
    RedisDatabase(const DatabaseConnectionInfo& connInfo);
    ~RedisDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;
    void checkRefreshWorkflowAsync();

    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow_.isRunning();
    }

    // Redis-specific key management (adapted to table interface)
    void checkTablesStatusAsync();

    // IQueryExecutor implementation
    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

    // Key data viewing (adapted to table interface)
    std::vector<std::vector<std::string>> getTableData(const std::string& keyPattern, int limit,
                                                       int offset);
    std::vector<std::string> getColumnNames(const std::string& keyPattern);
    int getRowCount(const std::string& keyPattern);

    // Redis-specific methods
    std::vector<RedisKey> getKeys(const std::string& pattern = "*", int limit = 1000) const;
    std::string getKeyValue(const std::string& key, const std::string& knownType = "") const;
    std::string getKeyType(const std::string& key) const;
    int64_t getKeyTTL(const std::string& key) const;

    // Redis database enumeration (db0-db15)
    bool selectDatabase(int dbIndex);
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

    // Async key loading (combines RedisNode functionality)
    void startKeysLoadAsync(bool forceRefresh = false);
    void checkKeysStatusAsync();
    std::vector<Table> getKeysAsync();

    // Key groups access
    const std::vector<Table>& getKeyGroups() const {
        return tables;
    }

    // Async operation status
    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || loadingKeys.load() || loadingDbInfo_.load() ||
               refreshWorkflow_.isRunning();
    }

    // Loading state (public like SQLite)
    std::atomic<bool> loadingKeys = false;
    bool keysLoaded = false;
    std::string lastKeysError;

protected:
    std::vector<std::string> getTableNames(); // Will return key patterns

private:
    // Redis-specific state (base class handles common state)
    mutable std::mutex contextMutex_;
    redisContext* context = nullptr;
    redisSSLContext* sslCtx_ = nullptr;

    AsyncOperation<bool> refreshWorkflow_;

    // Async key loading
    AsyncOperation<std::vector<Table>> keysLoadOp_;
    std::vector<Table> tables;

    // Database enumeration (db0-db15)
    int selectedDbIndex_ = 0;
    int numDatabases_ = 16; // default Redis has 16 databases
    std::vector<RedisDbInfo> dbInfoList_;
    std::atomic<bool> loadingDbInfo_ = false;
    bool dbInfoLoaded_ = false;
    AsyncOperation<std::vector<RedisDbInfo>> dbInfoLoadOp_;
    std::vector<RedisDbInfo> fetchDatabaseInfo();

    // Helper methods
    redisReply* executeRedisCommand(const std::string& command) const;
    redisReply* executeRedisCommandParsed(const std::vector<std::string>& commandParts) const;
    static std::string formatRedisReply(redisReply* reply);
    static std::vector<std::string> parseRedisCommand(const std::string& command);
    void groupKeysByPattern(std::vector<Table>& out) const;
};
