#pragma once

#include "async_helper.hpp"
#include "base_database.hpp"
#include "mysql/mysql_database_node.hpp"
#include <mutex>
#include <soci/connection-pool.h>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>
#include <unordered_map>

class MySQLDatabase final : public BaseDatabaseImpl {
    friend class MySQLDatabaseNode;

public:
    MySQLDatabase(const DatabaseConnectionInfo& connInfo);
    ~MySQLDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // Query execution
    std::string executeQuery(const std::string& query) override;

    // Database list methods
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

protected:
    // Async loading helpers
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;

    // Async database loading
    AsyncOperation<std::vector<std::string>> databasesLoader;

    // Async refresh workflow (for sequential operations)
    AsyncOperation<bool> refreshWorkflow;

public:
    // Helper methods for per-database data access
    MySQLDatabaseNode* getDatabaseData(const std::string& dbName);

    // Accessor for database data map (used by new hierarchy)
    // Auto-loads databases if not loaded and not currently loading
    std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

private:
    // Thread synchronization
    mutable std::mutex sessionMutex;

    // Helper methods for connection pool
    soci::connection_pool* getConnectionPoolForDatabase(const std::string& dbName) const;
    void ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info);

    // Helper method for session management
    std::unique_ptr<soci::session> getSession(const std::string& dbName = "") const;
};
