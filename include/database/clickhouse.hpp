#pragma once

#include "async_helper.hpp"
#include "clickhouse/clickhouse_database_node.hpp"
#include "connection_pool.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

class ClickHouseDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class ClickHouseDatabaseNode;

public:
    ClickHouseDatabase(const DatabaseConnectionInfo& connInfo);
    ~ClickHouseDatabase() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    std::pair<bool, std::string> createDatabase(const std::string& dbName,
                                                const std::string& comment = "") override;
    std::pair<bool, std::string>
    createDatabaseWithOptions(const CreateDatabaseOptions& options) override;
    std::pair<bool, std::string> renameDatabase(const std::string& oldName,
                                                const std::string& newName) override;
    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

    void refreshDatabaseNames();

    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow.isRunning();
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

    [[nodiscard]] bool hasPendingAsyncWork() const override;

    ClickHouseDatabaseNode* getDatabaseData(const std::string& dbName);
    std::unordered_map<std::string, std::unique_ptr<ClickHouseDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<ClickHouseDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

protected:
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    std::unordered_map<std::string, std::unique_ptr<ClickHouseDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;
    std::vector<std::string> pendingRefreshDatabaseNames;
    mutable std::mutex refreshStateMutex;

    AsyncOperation<std::vector<std::string>> databasesLoader;
    AsyncOperation<bool> refreshWorkflow;

    mutable std::mutex sessionMutex;

    void ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info);
    ConnectionPool<CHClientHandle>::Session getSession() const;
};
