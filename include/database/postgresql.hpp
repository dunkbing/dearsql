#pragma once

#include "async_helper.hpp"
#include "base_database.hpp"
#include "postgres/postgres_database_node.hpp"
#include "postgres/postgres_schema_node.hpp"
#include <mutex>
#include <soci/connection-pool.h>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <unordered_map>

class PostgresDatabase final : public BaseDatabaseImpl {
    friend class PostgresDatabaseNode;

public:
    PostgresDatabase(const DatabaseConnectionInfo& connInfo);
    ~PostgresDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    void refreshDatabaseNames();
    bool shouldShowAllDatabases() const {
        return connectionInfo.showAllDatabases;
    }

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

    // Query execution
    std::string executeQuery(const std::string& query) override;

protected:
    // Async loading helpers
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;

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
    soci::connection_pool* getConnectionPoolForDatabase(const std::string& dbName) const;
    void ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info);

    // Helper method for session management
    std::unique_ptr<soci::session> getSession(const std::string& dbName = "") const;
    void triggerChildDbRefresh();
};
