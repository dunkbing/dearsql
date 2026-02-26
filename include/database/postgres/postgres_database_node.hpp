#pragma once

#include "database/async_helper.hpp"
#include "database/connection_pool.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/query_executor.hpp"
#include "postgres_schema_node.hpp"
#include <libpq-fe.h>
#include <memory>
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
class PostgresDatabaseNode : public IQueryExecutor {
public:
    PostgresDatabase* parentDb = nullptr;

    std::string name;

    // Connection pool (one per database)
    std::unique_ptr<ConnectionPool<PGconn*>> connectionPool;

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
    ConnectionPool<PGconn*>::Session getSession() const;
    void initializeConnectionPool(const DatabaseConnectionInfo& info);

    // query execution with comprehensive result
    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

    // database operations (schema-aware)
    std::vector<std::vector<std::string>>
    getTableData(const std::string& schemaName, const std::string& tableName, int limit, int offset,
                 const std::string& whereClause = "", const std::string& orderByClause = "");
    std::vector<std::string> getColumnNames(const std::string& schemaName,
                                            const std::string& tableName);
    int getRowCount(const std::string& schemaName, const std::string& tableName,
                    const std::string& whereClause = "");

private:
    bool refreshChildrenAfterSchemasLoad = false;

    // cached aggregated tables/views across all schemas
    mutable std::vector<Table> allTables;
    mutable std::vector<Table> allViews;
    mutable bool allTablesCached = false;
    mutable bool allViewsCached = false;

    // internal method to refresh all child schemas (tables, views, sequences)
    void triggerChildSchemaRefresh();
};
