#pragma once

#include "db_interface.hpp"
#include <atomic>
#include <future>
#include <mutex>
#include <set>
#include <soci/connection-pool.h>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <unordered_map>

class PostgresDatabase final : public DatabaseInterface {
public:
    PostgresDatabase(const std::string& name, const std::string& host, int port,
                     const std::string& database, const std::string& username,
                     const std::string& password, bool showAllDatabases = false);
    ~PostgresDatabase() override;

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
    const std::string& getPath() const override;
    void* getConnection() const override;
    DatabaseType getType() const override;
    const std::string& getDatabaseName() const;

    // Table management
    void refreshTables() override;
    const std::vector<Table>& getTables() const override;
    std::vector<Table>& getTables() override;
    bool areTablesLoaded() const override;
    void setTablesLoaded(bool loaded) override;
    bool isLoadingTables() const override;
    void checkTablesStatusAsync() override;

    // View management
    void refreshViews() override;
    const std::vector<Table>& getViews() const override;
    std::vector<Table>& getViews() override;
    bool areViewsLoaded() const override;
    void setViewsLoaded(bool loaded) override;
    bool isLoadingViews() const override;
    void checkViewsStatusAsync() override;

    // Sequence management
    void refreshSequences() override;
    const std::vector<std::string>& getSequences() const override;
    std::vector<std::string>& getSequences() override;
    bool areSequencesLoaded() const override;
    void setSequencesLoaded(bool loaded) override;
    bool isLoadingSequences() const override;
    void checkSequencesStatusAsync() override;

    // Schema management
    void refreshSchemas();
    const std::vector<Schema>& getSchemas() const;
    std::vector<Schema>& getSchemas();
    bool areSchemasLoaded() const;
    void setSchemasLoaded(bool loaded);
    bool isLoadingSchemas() const;
    void checkSchemasStatusAsync();
    void checkSchemasStatusAsync(const std::string& dbName);
    void startSchemasLoadAsync(const std::string& dbName);

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
    void setDatabaseExpanded(const std::string& dbName, bool expanded_);

    // Query execution
    std::string executeQuery(const std::string& query) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName) override;

    // Async table data loading
    void startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                 const std::string& whereClause = "") override;
    bool isLoadingTableData(const std::string& tableName) const override;
    void checkTableDataStatusAsync(const std::string& tableName) override;
    bool hasTableDataResult(const std::string& tableName) const override;
    std::vector<std::vector<std::string>> getTableDataResult(const std::string& tableName) override;
    std::vector<std::string> getColumnNamesResult(const std::string& tableName) override;
    int getRowCountResult(const std::string& tableName) override;
    void clearTableDataResult(const std::string& tableName) override;

    // Legacy methods for backward compatibility
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
    void startRefreshSequenceAsync();
    std::vector<std::string> getSequencesAsync() const;
    void startRefreshSchemaAsync();
    std::vector<Schema> getSchemasAsync() const;
    std::vector<Schema> getSchemasForDatabaseAsync(const std::string& dbName) const;
    void startRefreshDatabasesAsync();
    std::vector<std::string> getDatabaseNamesAsync() const;

    // Schema helper methods
    std::vector<std::string> getSchemaNames() const;

private:
    std::string name;
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
        std::vector<std::string> sequences;
        std::vector<Schema> schemas;
        bool tablesLoaded = false;
        bool viewsLoaded = false;
        bool sequencesLoaded = false;
        bool schemasLoaded = false;
        bool tablesExpanded = false;
        bool viewsExpanded = false;
        bool sequencesExpanded = false;
        std::atomic<bool> loadingTables = false;
        std::atomic<bool> loadingViews = false;
        std::atomic<bool> loadingSequences = false;
        std::atomic<bool> loadingSchemas = false;
        std::future<std::vector<Table>> tablesFuture;
        std::future<std::vector<Table>> viewsFuture;
        std::future<std::vector<std::string>> sequencesFuture;
        std::future<std::vector<Schema>> schemasFuture;
    };

    std::unordered_map<std::string, DatabaseData> databaseDataCache;
    std::vector<std::string> availableDatabases;
    std::set<std::string> expandedDatabases; // Track which databases have been expanded
    bool connected = false;
    bool expanded = false;
    bool databasesLoaded = false;
    bool attemptedConnection = false;
    std::string lastConnectionError;

    // Async database loading
    std::atomic<bool> loadingDatabases = false;
    std::future<std::vector<std::string>> databasesFuture;

    // Async database switching
    std::atomic<bool> switchingDatabase = false;
    std::string targetDatabaseName;
    std::future<std::pair<bool, std::string>> databaseSwitchFuture;

    // Per-database schema loading (for parallel loading without switching)
    std::unordered_map<std::string, std::future<std::vector<Schema>>> databaseSchemaFutures;

public:
    // Helper methods for per-database data access
    DatabaseData& getCurrentDatabaseData();
    const DatabaseData& getCurrentDatabaseData() const;
    DatabaseData& getDatabaseData(const std::string& dbName);
    const DatabaseData& getDatabaseData(const std::string& dbName) const;

private:
    // Async connection
    std::atomic<bool> connecting = false;
    std::future<std::pair<bool, std::string>> connectionFuture;

    // Async table data loading - per table
    struct TableDataLoadState {
        std::atomic<bool> loading = false;
        std::atomic<bool> ready = false;
        std::vector<std::vector<std::string>> tableData;
        std::vector<std::string> columnNames;
        int rowCount = 0;
        std::future<void> future;
    };
    std::unordered_map<std::string, TableDataLoadState> tableDataStates;

    // Thread synchronization
    mutable std::mutex sessionMutex;

    // Helper methods for connection pool
    soci::connection_pool* getConnectionPoolForDatabase(const std::string& dbName) const;
    std::string buildConnectionString(const std::string& dbName) const;
    void initializeConnectionPool(const std::string& dbName, const std::string& connStr);

    // Helper method for session management
    std::unique_ptr<soci::session> getSession(const std::string& dbName = "") const;
};
