#pragma once

#include "base_database.hpp"
#include <atomic>
#include <future>
#include <mutex>
#include <set>
#include <soci/connection-pool.h>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <unordered_map>

class PostgresDatabase final : public BaseDatabaseImpl {
public:
    PostgresDatabase(const std::string& name, const std::string& host, int port,
                     const std::string& database, const std::string& username,
                     const std::string& password, bool showAllDatabases = false);
    ~PostgresDatabase() override;

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
    void refreshTables(const std::string& schemaName);
    void refreshViews() override;
    void refreshViews(const std::string& schemaName);
    void refreshSequences() override;
    void refreshSequences(const std::string& schemaName);

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
    void checkTablesStatusAsync() override;
    void checkViewsStatusAsync() override;
    void checkSequencesStatusAsync() override;

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

    // Async table data loading (BaseDatabaseImpl provides implementation)
    void startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                 const std::string& whereClause = "") override;

protected:
    std::vector<std::string> getTableNames() override;
    std::vector<std::string> getTableNames(const std::string& schemaName);
    std::vector<Column> getTableColumns(const std::string& tableName) override;
    std::vector<Index> getTableIndexes(const std::string& tableName);
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName);
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName,
                                                const std::string& schemaName);
    std::vector<std::string> getViewNames() override;
    std::vector<std::string> getViewNames(const std::string& schemaName);
    std::vector<Column> getViewColumns(const std::string& viewName) override;
    std::vector<std::string> getSequenceNames() override;
    std::vector<std::string> getSequenceNames(const std::string& schemaName);

    // Async loading helpers
    void startRefreshTableAsync(const std::string& schemaName = "public");
    std::vector<Table> getTablesWithColumnsAsync(const std::string& schemaName);
    void startRefreshViewAsync(const std::string& schemaName = "public");
    std::vector<Table> getViewsWithColumnsAsync(const std::string& schemaName);
    void startRefreshSequenceAsync(const std::string& schemaName = "public");
    std::vector<std::string> getSequencesAsync(const std::string& schemaName) const;
    void startRefreshSchemaAsync();
    std::vector<Schema> getSchemasAsync() const;
    std::vector<Schema> getSchemasForDatabaseAsync(const std::string& dbName) const;
    void startRefreshDatabasesAsync();
    std::vector<std::string> getDatabaseNamesAsync() const;

    // Schema helper methods
    std::vector<std::string> getSchemaNames() const;

private:
    // PostgreSQL-specific connection details (base class handles common state)
    std::string host;
    int port;
    std::string database;
    std::string username;
    std::string password;
    std::string connectionString;
    bool showAllDatabases;
    std::unordered_map<std::string, std::unique_ptr<soci::connection_pool>> connectionPools;
    // Per-schema data within a database
    struct SchemaData {
        std::vector<Table> tables;
        std::vector<Table> views;
        std::vector<std::string> sequences;
        bool tablesLoaded = false;
        bool viewsLoaded = false;
        bool sequencesLoaded = false;
        bool tablesExpanded = false;
        bool viewsExpanded = false;
        bool sequencesExpanded = false;
        std::atomic<bool> loadingTables = false;
        std::atomic<bool> loadingViews = false;
        std::atomic<bool> loadingSequences = false;
        std::future<std::vector<Table>> tablesFuture;
        std::future<std::vector<Table>> viewsFuture;
        std::future<std::vector<std::string>> sequencesFuture;
    };

    // Per-database data structures
    struct DatabaseData {
        std::vector<Schema> schemas;
        bool schemasLoaded = false;
        std::atomic<bool> loadingSchemas = false;
        std::future<std::vector<Schema>> schemasFuture;

        // Schema-level data cache (key: schema name)
        std::unordered_map<std::string, SchemaData> schemaDataCache;

        // UI state for backward compatibility (used by hierarchy_helpers for non-schema views)
        bool tablesExpanded = false;
        bool viewsExpanded = false;
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

    // Per-database schema loading (for parallel loading without switching)
    std::unordered_map<std::string, std::future<std::vector<Schema>>> databaseSchemaFutures;

public:
    // Helper methods for per-database data access
    DatabaseData& getCurrentDatabaseData();
    const DatabaseData& getCurrentDatabaseData() const;
    DatabaseData& getDatabaseData(const std::string& dbName);
    const DatabaseData& getDatabaseData(const std::string& dbName) const;

    // Helper methods for per-schema data access
    SchemaData& getSchemaData(const std::string& schemaName);
    const SchemaData& getSchemaData(const std::string& schemaName) const;
    SchemaData& getSchemaData(const std::string& dbName, const std::string& schemaName);
    const SchemaData& getSchemaData(const std::string& dbName, const std::string& schemaName) const;

    // Check async status for schema-level operations
    void checkSchemaTablesStatusAsync(const std::string& schemaName);
    void checkSchemaViewsStatusAsync(const std::string& schemaName);
    void checkSchemaSequencesStatusAsync(const std::string& schemaName);

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
