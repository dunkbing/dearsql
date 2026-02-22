#pragma once

#include "async_helper.hpp"
#include "connection_pool.hpp"
#include "db_interface.hpp"
#include "postgres/postgres_database_node.hpp"
#include "postgres/postgres_schema_node.hpp"
#include "query_executor.hpp"
#include <libpq-fe.h>
#include <mutex>
#include <unordered_map>
#include <vector>

class PostgresDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class PostgresDatabaseNode;

public:
    PostgresDatabase(const DatabaseConnectionInfo& connInfo);
    ~PostgresDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // Database operations
    std::pair<bool, std::string> createDatabase(const std::string& dbName,
                                                const std::string& comment = "") override;
    std::pair<bool, std::string>
    createDatabaseWithOptions(const CreateDatabaseOptions& options) override;
    std::pair<bool, std::string> renameDatabase(const std::string& oldName,
                                                const std::string& newName) override;
    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    void refreshDatabaseNames();

    // Connection status
    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow.isRunning();
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

    // Async operation status
    [[nodiscard]] bool hasPendingAsyncWork() const override;

    // IQueryExecutor implementation
    std::vector<QueryResult> executeQuery(const std::string& query, int rowLimit = 1000) override;

protected:
    // Async loading helpers
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;
    std::vector<std::string> pendingRefreshDatabaseNames;
    mutable std::mutex refreshStateMutex;

    // Async database loading
    AsyncOperation<std::vector<std::string>> databasesLoader;

    // Async refresh workflow (for sequential operations)
    AsyncOperation<bool> refreshWorkflow;

public:
    // Helper methods for per-database data access
    PostgresDatabaseNode* getDatabaseData(const std::string& dbName);
    const PostgresDatabaseNode* getDatabaseData(const std::string& dbName) const;

    // Accessor for database data map (used by new hierarchy)
    // Auto-loads databases if not loaded and not currently loading
    const std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>>&
    getDatabaseDataMap();

private:
    // Thread synchronization
    mutable std::mutex sessionMutex;

    // Helper methods for connection pool
    ConnectionPool<PGconn*>* getConnectionPoolForDatabase(const std::string& dbName) const;
    void ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info);

    // Helper method for session management
    ConnectionPool<PGconn*>::Session getSession() const;
    void triggerChildDbRefresh();
};
