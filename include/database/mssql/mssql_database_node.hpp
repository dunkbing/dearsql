#pragma once

#include "database/async_helper.hpp"
#include "database/connection_pool.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "mssql_schema_node.hpp"
#include <memory>
#include <string>
#include <vector>

#include "database/mssql/mssql_fwd.hpp"

class MSSQLDatabase;

class MSSQLDatabaseNode : public IDatabaseNode {
public:
    MSSQLDatabase* parentDb = nullptr;
    std::string name;

    std::unique_ptr<ConnectionPool<DBPROCESS*>> connectionPool;

    // schema hierarchy: Database -> Schemas -> Tables/Views
    std::vector<std::unique_ptr<MSSQLSchemaNode>> schemas;
    bool schemasLoaded = false;
    AsyncOperation<std::vector<std::unique_ptr<MSSQLSchemaNode>>> schemasLoader;
    std::string lastSchemasError;

    bool expanded = false;

    // IDatabaseNode
    [[nodiscard]] std::string getName() const override {
        return name;
    }
    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override;
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::MSSQL;
    }

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;
    std::pair<bool, std::string> createTable(const Table& table) override;

    // aggregated accessors (across all schemas)
    std::vector<Table>& getTables() override;
    const std::vector<Table>& getTables() const override;
    std::vector<Table>& getViews() override;
    const std::vector<Table>& getViews() const override;

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause = "",
                                                       const std::string& orderBy = "") override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause = "") override;

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

    // schemas
    void startSchemasLoadAsync(bool forceRefresh = false, bool refreshChildren = false);
    void checkSchemasStatusAsync();

    // internal
    void ensureConnectionPool();
    ConnectionPool<DBPROCESS*>::Session getSession() const;
    void initializeConnectionPool(const DatabaseConnectionInfo& info);
    void invalidateAggregatedObjects() const;

private:
    bool refreshChildrenAfterSchemasLoad = false;
    mutable std::vector<Table> allTables;
    mutable std::vector<Table> allViews;
    mutable bool aggregatedObjectsDirty = true;
    mutable std::string aggregatedTablesError;
    mutable std::string aggregatedViewsError;

    void triggerChildSchemaRefresh();
    void rebuildAggregatedObjects() const;
    MSSQLSchemaNode* findSchema(const std::string& schemaName);
    const MSSQLSchemaNode* findSchema(const std::string& schemaName) const;
};
