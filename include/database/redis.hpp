#pragma once

#include "db_interface.hpp"
#include <atomic>
#include <future>
#include <hiredis.h>

struct RedisKey {
    std::string name;
    std::string type;
    std::string value;
    int64_t ttl = -1; // -1 means no expiration
};

class RedisDatabase final : public DatabaseInterface {
public:
    RedisDatabase(std::string name, std::string host, int port, std::string password = "",
                  std::string username = "");
    ~RedisDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect(bool forceRefresh = false) override;
    void disconnect() override;
    bool isConnected() const override;
    bool isConnecting() const override;
    void startConnectionAsync(bool forceRefresh = false) override;
    void checkConnectionStatusAsync() override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
    void* getConnection() const override;
    DatabaseType getType() const override;

    // Redis-specific key management (adapted to table interface)
    const std::vector<Table>& getTables() const;
    std::vector<Table>& getTables() override;
    bool isLoadingTables() const override;
    void checkTablesStatusAsync();

    // Views not applicable for Redis
    void refreshViews() override;

    // Sequences not applicable for Redis
    void refreshSequences() override;
    const std::vector<std::string>& getSequences() const override;
    std::vector<std::string>& getSequences() override;

    // Redis command execution (adapted to query interface)
    std::string executeQuery(const std::string& command) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& command) override;

    // Key data viewing (adapted to table interface)
    std::vector<std::vector<std::string>> getTableData(const std::string& keyPattern, int limit,
                                                       int offset);
    std::vector<std::string> getColumnNames(const std::string& keyPattern);
    int getRowCount(const std::string& keyPattern);

    // Connection attempt tracking
    bool hasAttemptedConnection() const override;
    void setAttemptedConnection(bool attempted) override;
    const std::string& getLastConnectionError() const override;
    void setLastConnectionError(const std::string& error) override;
    void setConnectionId(int id) override {
        savedConnectionId = id;
    }
    [[nodiscard]] int getConnectionId() const override {
        return savedConnectionId;
    }

    // Redis-specific methods
    std::vector<RedisKey> getKeys(const std::string& pattern = "*", int limit = 1000) const;
    std::string getKeyValue(const std::string& key) const;
    std::string getKeyType(const std::string& key) const;
    int64_t getKeyTTL(const std::string& key) const;

    // Connection detail getters
    const std::string& getHost() const {
        return host;
    }
    int getPort() const {
        return port;
    }
    const std::string& getUsername() const {
        return username;
    }
    const std::string& getPassword() const {
        return password;
    }

    // Async key loading (combines RedisNode functionality)
    void startKeysLoadAsync(bool forceRefresh = false);
    void checkKeysStatusAsync();
    std::vector<Table> getKeysAsync();

    // Key groups access
    const std::vector<Table>& getKeyGroups() const {
        return tables;
    }

    // Loading state (public like SQLite)
    std::atomic<bool> loadingKeys = false;
    bool keysLoaded = false;
    std::string lastKeysError;

protected:
    std::vector<std::string> getTableNames(); // Will return key patterns
    // std::vector<Column> getTableColumns(const std::string& keyPattern) override;

private:
    std::string name;
    std::string host;
    int port;
    std::string password;
    std::string username;
    std::string connectionString;
    redisContext* context = nullptr;
    std::vector<Table> tables;          // Will represent key groups
    std::vector<Table> views;           // Empty for Redis
    std::vector<std::string> sequences; // Empty for Redis
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;
    bool attemptedConnection = false;
    std::string lastConnectionError;
    int savedConnectionId = -1;

    // Async connection state
    bool connecting = false;
    std::future<std::pair<bool, std::string>> connectionFuture;

    // Async key loading future
    std::future<std::vector<Table>> keysFuture;

    // Helper methods
    redisReply* executeRedisCommand(const std::string& command) const;
    redisReply* executeRedisCommandParsed(const std::vector<std::string>& commandParts) const;
    static std::string formatRedisReply(redisReply* reply);
    static std::vector<std::string> parseRedisCommand(const std::string& command);
    void groupKeysByPattern();
};
