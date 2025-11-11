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

    // Database list methods
    [[deprecated("Use getDatabaseDataMap() instead")]]
    std::vector<std::string> getDatabases();
    void refreshDatabaseNames();
    bool shouldShowAllDatabases() const {
        return connectionInfo.showAllDatabases;
    }

    // Connection info getter/setter
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

    // Query execution
    std::string executeQuery(const std::string& query) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query) override;

protected:
    // std::vector<Column> getTableColumns(const std::string& tableName) override;
    std::vector<Index> getTableIndexes(const std::string& tableName);
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName);
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName,
                                                const std::string& schemaName);
    std::vector<std::string> getViewNames(const std::string& schemaName);
    std::vector<std::string> getSequenceNames(const std::string& schemaName);

    // Async loading helpers
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    // PostgreSQL-specific connection details (base class handles common state)
    DatabaseConnectionInfo connectionInfo;
    std::string database;
    std::string connectionString;

    std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>> databaseDataCache;
    std::vector<std::string> availableDatabases;
    std::set<std::string> expandedDatabases; // Track which databases have been expanded
    bool databasesLoaded = false;

    // Async database loading
    std::atomic<bool> loadingDatabases = false;
    std::future<std::vector<std::string>> databasesFuture;

    std::string targetDatabaseName;

public:
    // Helper methods for per-database data access
    PostgresDatabaseNode* getCurrentDatabaseData();
    const PostgresDatabaseNode* getCurrentDatabaseData() const;
    PostgresDatabaseNode* getDatabaseData(const std::string& dbName);
    const PostgresDatabaseNode* getDatabaseData(const std::string& dbName) const;

    // Accessor for database data map (used by new hierarchy)
    // Auto-loads databases if not loaded and not currently loading
    const std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>>&
    getDatabaseDataMap();

    // Helper methods for per-schema data access
    PostgresSchemaNode& getSchemaData(const std::string& schemaName);
    const PostgresSchemaNode& getSchemaData(const std::string& schemaName) const;
    PostgresSchemaNode& getSchemaData(const std::string& dbName, const std::string& schemaName);
    const PostgresSchemaNode& getSchemaData(const std::string& dbName,
                                            const std::string& schemaName) const;

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
