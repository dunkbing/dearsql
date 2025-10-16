#pragma once

#include "base_database.hpp"
#include "postgres/postgres_database_node.hpp"
#include "postgres/postgres_schema_node.hpp"
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
    PostgresDatabase(const DatabaseConnectionInfo& connInfo);
    ~PostgresDatabase() override;

    // Connection management (BaseDatabaseImpl handles common async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
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
    const std::vector<std::unique_ptr<PostgresSchemaNode>>& getSchemas() const;
    std::vector<std::unique_ptr<PostgresSchemaNode>>& getSchemas();
    bool areSchemasLoaded() const;
    void setSchemasLoaded(bool loaded);
    bool isLoadingSchemas() const;
    // void checkSchemasStatusAsync();
    void checkTablesStatusAsync() override;
    void checkViewsStatusAsync() override;
    void checkSequencesStatusAsync() override;

    // Database list methods
    [[deprecated("Use getDatabaseDataMap() instead")]]
    std::vector<std::string> getDatabases();
    void refreshDatabaseNames();
    bool shouldShowAllDatabases() const {
        return showAllDatabases;
    }

    // Connection info getter
    const DatabaseConnectionInfo& getConnectionInfo() const {
        return connectionInfo;
    }
    const std::string& getDatabase() const {
        return database;
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
    std::vector<std::unique_ptr<PostgresSchemaNode>> getSchemasAsync() const;
    std::vector<std::string> getDatabaseNamesAsync() const;

    // Schema helper methods
    std::vector<std::string> getSchemaNames() const;

private:
    // PostgreSQL-specific connection details (base class handles common state)
    DatabaseConnectionInfo connectionInfo;
    std::string database;
    std::string connectionString;
    bool showAllDatabases;

    std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>> databaseDataCache;
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
    PostgresDatabaseNode* getCurrentDatabaseData();
    const PostgresDatabaseNode* getCurrentDatabaseData() const;
    PostgresDatabaseNode* getDatabaseData(const std::string& dbName);
    const PostgresDatabaseNode* getDatabaseData(const std::string& dbName) const;

    // Accessor for database data map (used by new hierarchy)
    const std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

    // Helper methods for per-schema data access
    PostgresSchemaNode& getSchemaData(const std::string& schemaName);
    const PostgresSchemaNode& getSchemaData(const std::string& schemaName) const;
    PostgresSchemaNode& getSchemaData(const std::string& dbName, const std::string& schemaName);
    const PostgresSchemaNode& getSchemaData(const std::string& dbName,
                                            const std::string& schemaName) const;

    // Check async status for schema-level operations
    void checkSchemaTablesStatusAsync(const std::string& schemaName);
    void checkSchemaViewsStatusAsync(const std::string& schemaName);
    void checkSchemaSequencesStatusAsync(const std::string& schemaName);

    // Public helper for building connection strings (used by PostgresDatabaseNode)
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
