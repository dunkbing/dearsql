#pragma once

#include "db_interface.hpp"
#include "query_executor.hpp"
#include <atomic>
#include <future>
#include <hiredis/hiredis.h>
#include <hiredis/hiredis_ssl.h>

struct RedisKey {
    std::string name;
    std::string type;
    std::string value;
    int64_t ttl = -1; // -1 means no expiration
};

class RedisDatabase final : public DatabaseInterface, public IQueryExecutor {
public:
    RedisDatabase(const DatabaseConnectionInfo& connInfo);
    ~RedisDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

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
    std::string getKeyValue(const std::string& key) const;
    std::string getKeyType(const std::string& key) const;
    int64_t getKeyTTL(const std::string& key) const;

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
        return isConnecting() || loadingKeys.load();
    }

    // Loading state (public like SQLite)
    std::atomic<bool> loadingKeys = false;
    bool keysLoaded = false;
    std::string lastKeysError;

protected:
    std::vector<std::string> getTableNames(); // Will return key patterns

private:
    // Redis-specific state (base class handles common state)
    redisContext* context = nullptr;
    redisSSLContext* sslCtx_ = nullptr;

    // Async key loading future
    std::future<std::vector<Table>> keysFuture;
    std::vector<Table> tables;

    // Helper methods
    redisReply* executeRedisCommand(const std::string& command) const;
    redisReply* executeRedisCommandParsed(const std::vector<std::string>& commandParts) const;
    static std::string formatRedisReply(redisReply* reply);
    static std::vector<std::string> parseRedisCommand(const std::string& command);
    void groupKeysByPattern();
};
