#pragma once

#include "async_helper.hpp"
#include "cassandra/cassandra_database_node.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include <cassandra.h>
#include <mutex>
#include <unordered_map>
#include <vector>

class CassandraDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class CassandraDatabaseNode;

public:
    explicit CassandraDatabase(const DatabaseConnectionInfo& connInfo);
    ~CassandraDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // Database operations (keyspace = database in Cassandra)
    std::pair<bool, std::string> createDatabase(const std::string& dbName,
                                                const std::string& comment = "") override;
    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    // IQueryExecutor implementation: runs CQL against the session.
    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

    // Keyspace list
    void refreshDatabaseNames() override;

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

    // Helpers used by CassandraDatabaseNode
    CassSession* session() const {
        return session_;
    }

    CassandraDatabaseNode* getDatabaseData(const std::string& keyspace);

    std::unordered_map<std::string, std::unique_ptr<CassandraDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<CassandraDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

protected:
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    // libuv-backed driver state. Owned by this class; freed on destruction.
    CassCluster* cluster_ = nullptr;
    CassSession* session_ = nullptr;
    CassSsl* ssl_ = nullptr;
    mutable std::mutex sessionMutex_;

    std::unordered_map<std::string, std::unique_ptr<CassandraDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;
    std::vector<std::string> pendingRefreshDatabaseNames;
    mutable std::mutex refreshStateMutex;

    AsyncOperation<std::vector<std::string>> databasesLoader;
    AsyncOperation<bool> refreshWorkflow;

    // Apply SSL options derived from connectionInfo.sslmode to cluster_.
    std::pair<bool, std::string> applySslConfig();
    void freeDriverState();
};
