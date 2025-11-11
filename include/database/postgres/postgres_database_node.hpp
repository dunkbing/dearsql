#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "postgres_schema_node.hpp"
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
class PostgresDatabaseNode {
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
    AsyncOperation<std::vector<std::unique_ptr<PostgresSchemaNode>>> schemasLoader;
    std::string lastSchemasError;

    // UI expansion state
    bool expanded = false;
    bool tablesExpanded = false; // For backward compatibility
    bool viewsExpanded = false;  // For backward compatibility

    // Methods
    void startSchemasLoadAsync(bool forceRefresh = false, bool refreshChildren = false);
    void checkSchemasStatusAsync();
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

private:
    // cached aggregated tables/views across all schemas
    mutable std::vector<Table> allTables;
    mutable std::vector<Table> allViews;
    mutable bool allTablesCached = false;
    mutable bool allViewsCached = false;

    // internal method to refresh all child schemas (tables, views, sequences)
    void triggerChildSchemaRefresh();
};
