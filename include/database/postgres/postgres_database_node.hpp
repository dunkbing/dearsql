#pragma once

#include "database/async_helper.hpp"
#include "database/connection_pool.hpp"
#include "database/database_node.hpp"
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
class PostgresDatabaseNode : public IDatabaseNode {
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

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getName() const override {
        return name;
    }
    [[nodiscard]] std::string getFullPath() const override;
    [[nodiscard]] DatabaseType getDatabaseType() const override;

    std::vector<Table>& getTables() override;
    [[nodiscard]] const std::vector<Table>& getTables() const override;
    std::vector<Table>& getViews() override;
    [[nodiscard]] const std::vector<Table>& getViews() const override;
    [[nodiscard]] const std::vector<std::string>& getSequences() const override;

    std::vector<std::vector<std::string>>
    getTableData(const std::string& tableName, int limit, int offset,
                 const std::string& whereClause = "",
                 const std::string& orderByClause = "") override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName, const std::string& whereClause = "") override;

    [[nodiscard]] bool isTablesLoaded() const override;
    [[nodiscard]] bool isViewsLoaded() const override;
    [[nodiscard]] bool isLoadingTables() const override;
    [[nodiscard]] bool isLoadingViews() const override;

    void startTablesLoadAsync(bool force = false) override;
    void startViewsLoadAsync(bool force = false) override;
    void checkLoadingStatus() override;

    [[nodiscard]] const std::string& getLastTablesError() const override;
    [[nodiscard]] const std::string& getLastViewsError() const override;

    void startTableRefreshAsync(const std::string& tableName) override;
    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override;
    void checkTableRefreshStatusAsync(const std::string& tableName) override;

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
    mutable std::vector<Table> allTables;
    mutable std::vector<Table> allViews;
    mutable std::vector<std::string> allSequences;
    mutable bool aggregatedObjectsDirty = true;
    mutable std::string aggregatedTablesError;
    mutable std::string aggregatedViewsError;

    // internal method to refresh all child schemas (tables, views, sequences)
    void triggerChildSchemaRefresh();
    void invalidateAggregatedObjects() const;
    void rebuildAggregatedObjects() const;
};
