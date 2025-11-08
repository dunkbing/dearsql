#pragma once

#include "database/db.hpp"
#include "database/table_data_provider.hpp"
#include "postgres_schema_node.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <soci/connection-pool.h>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration
class PostgresDatabase;

/**
 * @brief Per-database data for PostgreSQL
 *
 * PostgreSQL hierarchy: Server → Databases → (app_db, reporting_db, ...) → Schemas
 * Each PostgresDatabaseNode represents one database within the PostgreSQL server.
 */
class PostgresDatabaseNode : public ITableDataProvider {
public:
    PostgresDatabase* parentDb = nullptr;

    std::string name;

    // Connection pool (one per database)
    std::unique_ptr<soci::connection_pool> connectionPool;

    // PostgreSQL: Database → Schemas → Tables/Views/Sequences
    std::vector<std::unique_ptr<PostgresSchemaNode>> schemas;
    // deprecated
    std::unordered_map<std::string, std::unique_ptr<PostgresSchemaNode>> schemaDataCache;
    bool schemasLoaded = false;
    std::atomic<bool> loadingSchemas = false;
    std::future<std::vector<std::unique_ptr<PostgresSchemaNode>>> schemasFuture;
    std::string lastSchemasError;

    // UI expansion state
    bool expanded = false;
    bool tablesExpanded = false; // For backward compatibility
    bool viewsExpanded = false;  // For backward compatibility

    // Methods
    void startSchemasLoadAsync(bool forceRefresh = false);
    void checkSchemasStatusAsync();
    std::vector<std::unique_ptr<PostgresSchemaNode>> getSchemasForDatabaseAsync();
    std::unique_ptr<soci::session> getSession() const;
    void initializeConnectionPool(const std::string& connStr);

    // query execution with comprehensive result
    QueryResult executeQueryWithResult(const std::string& query, int rowLimit = 1000);

    // database operations (schema-aware)
    std::vector<std::vector<std::string>> getTableData(const std::string& schemaName,
                                                       const std::string& tableName, int limit,
                                                       int offset,
                                                       const std::string& whereClause = "");
    std::vector<std::string> getColumnNames(const std::string& schemaName,
                                            const std::string& tableName);
    int getRowCount(const std::string& schemaName, const std::string& tableName,
                    const std::string& whereClause = "");

    // ITableDataProvider interface implementation
    // tableName format: "schema.table" or just "table" (uses public schema)
    std::vector<std::vector<std::string>>
    getTableData(const std::string& tableName, int limit, int offset,
                 const std::string& whereClause = "") override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName, const std::string& whereClause = "") override;
    std::string executeQuery(const std::string& query) override;
    const std::vector<Table>& getTables() const override;
    const std::vector<Table>& getViews() const override;

private:
    // helper to parse "schema.table" format
    std::pair<std::string, std::string> parseSchemaTable(const std::string& qualifiedName) const;

    // cached aggregated tables/views across all schemas
    mutable std::vector<Table> allTables;
    mutable std::vector<Table> allViews;
    mutable bool allTablesCached = false;
    mutable bool allViewsCached = false;
};
