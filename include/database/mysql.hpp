#pragma once

#include "async_helper.hpp"
#include "db_interface.hpp"
#include "mysql/mysql_database_node.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <soci/connection-pool.h>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>
#include <unordered_map>

class MySQLDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class MySQLDatabaseNode;

public:
    MySQLDatabase(const DatabaseConnectionInfo& connInfo);
    ~MySQLDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // Database operations
    std::pair<bool, std::string> renameDatabase(const std::string& oldName,
                                                const std::string& newName) override;
    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    // IQueryExecutor implementation
    QueryResult executeQueryWithResult(const std::string& query, int rowLimit = 1000) override;

    // Database list methods
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

    // Helper methods for connection pool and session management
    void ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info);
    std::unique_ptr<soci::session> getSession() const;
};
