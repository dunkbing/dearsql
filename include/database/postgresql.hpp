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
    // Per-database data structures (made public for hierarchy rendering)
public:
    /**
     * @brief Per-schema data for PostgreSQL
     *
     * PostgreSQL hierarchy: Database → Schema → Tables/Views/Sequences
     * Each SchemaData represents one schema (e.g., "public", "analytics")
     */
    struct SchemaData {
        std::string name;

        // Schema contents (only tables, views, sequences for now)
        std::vector<Table> tables;
        std::vector<Table> views;
        std::vector<std::string> sequences;

        // Loading state flags
        bool tablesLoaded = false;
        bool viewsLoaded = false;
        bool sequencesLoaded = false;
        std::atomic<bool> loadingTables = false;
        std::atomic<bool> loadingViews = false;
        std::atomic<bool> loadingSequences = false;

        // Async futures
        std::future<std::vector<Table>> tablesFuture;
        std::future<std::vector<Table>> viewsFuture;
        std::future<std::vector<std::string>> sequencesFuture;

        // UI expansion state
        bool tablesExpanded = false;
        bool viewsExpanded = false;
        bool sequencesExpanded = false;

        // Error tracking
        std::string lastTablesError;
        std::string lastViewsError;
        std::string lastSequencesError;
    };

    /**
     * @brief Per-database data for PostgreSQL
     *
     * PostgreSQL hierarchy: Server → Databases → (app_db, reporting_db, ...) → Schemas
     * Each DatabaseData represents one database within the PostgreSQL server.
     */
    struct DatabaseData {
        PostgresDatabase* parentDb = nullptr;

        std::string name;

        // Connection pool (one per database)
        std::unique_ptr<soci::connection_pool> connectionPool;

        // PostgreSQL: Database → Schemas → Tables/Views/Sequences
        std::vector<Schema> schemas; // List of schema names (for legacy compatibility)
        std::unordered_map<std::string, SchemaData> schemaDataCache; // Schema name → data
        bool schemasLoaded = false;
        std::atomic<bool> loadingSchemas = false;
        std::future<std::vector<Schema>> schemasFuture;
        std::string lastSchemasError;

        // UI expansion state
        bool expanded = false;
        bool tablesExpanded = false; // For backward compatibility
        bool viewsExpanded = false;  // For backward compatibility

        // Methods
        void startSchemasLoadAsync();
        void checkSchemasStatusAsync();
        std::vector<Schema> getSchemasForDatabaseAsync();
        std::unique_ptr<soci::session> getSession() const;
        void initializeConnectionPool(const std::string& connStr);
    };

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
    const std::vector<Schema>& getSchemas() const;
    std::vector<Schema>& getSchemas();
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
    std::vector<Schema> getSchemasAsync() const;
    std::vector<std::string> getDatabaseNamesAsync() const;

    // Schema helper methods
    std::vector<std::string> getSchemaNames() const;

private:
    // PostgreSQL-specific connection details (base class handles common state)
    DatabaseConnectionInfo connectionInfo;
    std::string database;
    std::string connectionString;
    bool showAllDatabases;
    // Note: connectionPools removed - now stored in DatabaseData::connectionPool

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

    // Accessor for database data map (used by new hierarchy)
    const std::unordered_map<std::string, DatabaseData>& getDatabaseDataMap() const {
        return databaseDataCache;
    }

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
