#pragma once

#include "base_database.hpp"
#include <atomic>
#include <future>
#include <mutex>
#include <set>
#include <soci/connection-pool.h>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>
#include <unordered_map>

class MySQLDatabase final : public BaseDatabaseImpl {
public:
    MySQLDatabase(const std::string& name, const std::string& host, int port,
                  const std::string& database, const std::string& username,
                  const std::string& password, bool showAllDatabases = false);
    ~MySQLDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
    const std::string& getPath() const override;
    void* getConnection() const override;
    DatabaseType getType() const override;
    const std::string& getDatabaseName() const;

    // Schema management (BaseDatabaseImpl provides base getters/setters)
    void refreshTables() override;
    void refreshViews() override;
    void refreshSequences() override;
    void checkTablesStatusAsync() override;
    void checkViewsStatusAsync() override;
    void checkSequencesStatusAsync() override;

    // Query execution
    std::string executeQuery(const std::string& query) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName) override;

    // Async table data loading (BaseDatabaseImpl provides implementation)
    void startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                 const std::string& whereClause = "") override;

    // Database list methods
    std::vector<std::string> getDatabaseNames();
    void refreshDatabaseNames();
    bool shouldShowAllDatabases() const {
        return showAllDatabases;
    }

    // Connection detail getters
    const std::string& getHost() const {
        return host;
    }
    int getPort() const {
        return port;
    }
    const std::string& getDatabase() const {
        return database;
    }
    const std::string& getUsername() const {
        return username;
    }
    const std::string& getPassword() const {
        return password;
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    std::pair<bool, std::string> switchToDatabase(const std::string& targetDatabase);
    void switchToDatabaseAsync(const std::string& targetDatabase);
    bool isSwitchingDatabase() const;
    void checkDatabaseSwitchStatusAsync();
    bool isDatabaseExpanded(const std::string& dbName) const;
    void setDatabaseExpanded(const std::string& dbName, bool expanded);

protected:
    std::vector<std::string> getTableNames() override;
    std::vector<Column> getTableColumns(const std::string& tableName) override;
    std::vector<Index> getTableIndexes(const std::string& tableName);
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName);
    std::vector<std::string> getViewNames() override;
    std::vector<Column> getViewColumns(const std::string& viewName) override;
    std::vector<std::string> getSequenceNames() override;

    // Async loading helpers
    void startRefreshTableAsync();
    std::vector<Table> getTablesWithColumnsAsync();
    void startRefreshViewAsync();
    std::vector<Table> getViewsWithColumnsAsync();
    void startRefreshDatabasesAsync();
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    // MySQL-specific connection details (base class handles common state)
    std::string host;
    int port;
    std::string database;
    std::string username;
    std::string password;
    std::string connectionString;
    bool showAllDatabases;
    std::unordered_map<std::string, std::unique_ptr<soci::connection_pool>> connectionPools;
    // Per-database data structures
    struct DatabaseData {
        std::vector<Table> tables;
        std::vector<Table> views;
        std::vector<std::string> sequences; // Empty for MySQL
        bool tablesLoaded = false;
        bool viewsLoaded = false;
        bool sequencesLoaded = false;
        bool tablesExpanded = false;
        bool viewsExpanded = false;
        bool sequencesExpanded = false;
        std::atomic<bool> loadingTables = false;
        std::atomic<bool> loadingViews = false;
        std::future<std::vector<Table>> tablesFuture;
        std::future<std::vector<Table>> viewsFuture;
    };

    std::unordered_map<std::string, DatabaseData> databaseDataCache;
    std::vector<std::string> availableDatabases;
    std::set<std::string> expandedDatabases; // Track which databases have been expanded
    bool databasesLoaded = false;

    // Async database loading
    std::atomic<bool> loadingDatabases = false;
    std::future<std::vector<std::string>> databasesFuture;

    // Async database switching
    std::atomic<bool> switchingDatabase = false;
    std::string targetDatabaseName;
    std::future<std::pair<bool, std::string>> databaseSwitchFuture;

public:
    // Helper methods for per-database data access
    DatabaseData& getCurrentDatabaseData();
    const DatabaseData& getCurrentDatabaseData() const;
    DatabaseData& getDatabaseData(const std::string& dbName);
    const DatabaseData& getDatabaseData(const std::string& dbName) const;

private:
    // Thread synchronization
    mutable std::mutex sessionMutex;

    // Helper methods for connection pool
    soci::connection_pool* getConnectionPoolForDatabase(const std::string& dbName) const;
    std::string buildConnectionString(const std::string& dbName) const;
    void initializeConnectionPool(const std::string& dbName, const std::string& connStr);

    // Helper method for session management
    std::unique_ptr<soci::session> getSession(const std::string& dbName = "") const;
};
