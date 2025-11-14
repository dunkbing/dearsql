#pragma once

#include "async_helper.hpp"
#include "base_database.hpp"
#include "mysql/mysql_database_node.hpp"
#include <mutex>
#include <set>
#include <soci/connection-pool.h>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>
#include <unordered_map>

class MySQLDatabase final : public BaseDatabaseImpl {
public:
    MySQLDatabase(const DatabaseConnectionInfo& connInfo);
    ~MySQLDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
    void* getConnection() const override;
    DatabaseType getType() const override;
    const std::string& getDatabaseName() const;

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

    const DatabaseConnectionInfo& getConnectionInfo() const {
        return connectionInfo;
    }
    void setConnectionInfo(const DatabaseConnectionInfo& info);
    const std::string& getDatabase() const {
        return database;
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

protected:
    // std::vector<Column> getTableColumns(const std::string& tableName) override;
    std::vector<Index> getTableIndexes(const std::string& tableName);
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName);

    // Async loading helpers
    void startRefreshTableAsync();
    void startRefreshViewAsync();
    std::vector<Table> getViewsWithColumnsAsync();
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    // MySQL-specific connection details (base class handles common state)
    DatabaseConnectionInfo connectionInfo;
    std::string database;
    std::string connectionString;

    std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>> databaseDataCache;
    std::set<std::string> expandedDatabases; // Track which databases have been expanded
    bool databasesLoaded = false;

    // Async database loading
    AsyncOperation<std::vector<std::string>> databasesLoader;

    // Async refresh workflow (for sequential operations)
    AsyncOperation<bool> refreshWorkflow;

    // Async database switching
    std::string targetDatabaseName;

public:
    // Helper methods for per-database data access
    MySQLDatabaseNode* getCurrentDatabaseData();
    const MySQLDatabaseNode* getCurrentDatabaseData() const;
    MySQLDatabaseNode* getDatabaseData(const std::string& dbName);

    // Accessor for database data map (used by new hierarchy)
    // Auto-loads databases if not loaded and not currently loading
    std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<MySQLDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

    // Public helper for building connection strings (used by MySQLDatabaseNode)
    std::string buildConnectionString(const std::string& dbName) const;

private:
    // Thread synchronization
    mutable std::mutex sessionMutex;

    // Helper methods for connection pool
    soci::connection_pool* getConnectionPoolForDatabase(const std::string& dbName) const;
    void initializeConnectionPool(const std::string& dbName, const std::string& connStr);

    // Helper method for session management
    std::unique_ptr<soci::session> getSession(const std::string& dbName = "") const;
};
