#pragma once

#include "db_interface.hpp"
#include <future>
#include <hiredis.h>
#include <memory>

// Forward declaration
class RedisNode;

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
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool isConnecting() const override;
    void startConnectionAsync() override;
    void checkConnectionStatusAsync() override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
    void* getConnection() const override;
    DatabaseType getType() const override;

    // Redis-specific key management (adapted to table interface)
    void refreshTables() override; // Will load keys
    const std::vector<Table>& getTables() const override;
    std::vector<Table>& getTables() override;
    bool areTablesLoaded() const override;
    void setTablesLoaded(bool loaded) override;
    bool isLoadingTables() const override;
    void checkTablesStatusAsync();

    // Views not applicable for Redis
    void refreshViews() override;
    const std::vector<Table>& getViews() const override;
    std::vector<Table>& getViews() override;
    bool areViewsLoaded() const override;
    void setViewsLoaded(bool loaded) override;

    // Sequences not applicable for Redis
    void refreshSequences() override;
    const std::vector<std::string>& getSequences() const override;
    std::vector<std::string>& getSequences() override;
    bool areSequencesLoaded() const override;
    void setSequencesLoaded(bool loaded) override;

    // Redis command execution (adapted to query interface)
    std::string executeQuery(const std::string& command) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& command) override;

    // Key data viewing (adapted to table interface)
    std::vector<std::vector<std::string>> getTableData(const std::string& keyPattern, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string& keyPattern) override;
    int getRowCount(const std::string& keyPattern) override;

    // Async key data loading
    void startTableDataLoadAsync(const std::string& keyPattern, int limit, int offset,
                                 const std::string& whereClause = "") override;
    bool isLoadingTableData() const override;
    void checkTableDataStatusAsync() override;
    bool hasTableDataResult() const override;
    std::vector<std::vector<std::string>> getTableDataResult() override;
    std::vector<std::string> getColumnNamesResult() override;
    int getRowCountResult() override;
    void clearTableDataResult() override;

    // UI state
    bool isExpanded() const override;
    void setExpanded(bool expanded) override;

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

    // Node access
    std::shared_ptr<RedisNode> getRedisNode() const;

protected:
    std::vector<std::string> getTableNames(); // Will return key patterns
    std::vector<Column> getTableColumns(const std::string& keyPattern) override;
    std::vector<std::string> getViewNames() override;
    std::vector<Column> getViewColumns(const std::string& viewName) override;
    std::vector<std::string> getSequenceNames() override;

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

    // Async table loading state
    bool loadingTables = false;
    std::future<void> tablesFuture;

    TableDataLoader tableDataLoader;

    // Redis node (represents the connection)
    std::shared_ptr<RedisNode> redisNode;

    // Helper methods
    redisReply* executeRedisCommand(const std::string& command) const;
    redisReply* executeRedisCommandParsed(const std::vector<std::string>& commandParts) const;
    static std::string formatRedisReply(redisReply* reply);
    static std::vector<std::string> parseRedisCommand(const std::string& command);
    void groupKeysByPattern();
};
