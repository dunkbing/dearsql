#pragma once

#include "database/async_helper.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <string>
#include <vector>

// Forward declaration
class PostgresDatabaseNode;

/**
 * @brief Per-schema data for PostgreSQL
 *
 * PostgreSQL hierarchy: Database → Schema → Tables/Views/Sequences
 * Each PostgresSchemaNode represents one schema (e.g., "public", "analytics")
 */
class PostgresSchemaNode : public IDatabaseNode, public ITableDataProvider {
public:
    PostgresDatabaseNode* parentDbNode = nullptr;
    std::string name;

    // Schema contents
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<Table> materializedViews;
    std::vector<std::string> sequences;

    // Loading state flags
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool materializedViewsLoaded = false;
    bool sequencesLoaded = false;

    // Async operations
    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    AsyncOperation<std::vector<Table>> materializedViewsLoader;
    AsyncOperation<std::vector<std::string>> sequencesLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastMaterializedViewsError;
    std::string lastSequencesError;

    // ========== IDatabaseNode Implementation ==========

    [[nodiscard]] std::string getName() const override {
        return name;
    }

    [[nodiscard]] std::string getFullPath() const override;

    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::POSTGRESQL;
    }

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;
    std::pair<bool, std::string> createTable(const Table& table) override;

    std::vector<Table>& getTables() override {
        return tables;
    }
    const std::vector<Table>& getTables() const override {
        return tables;
    }

    std::vector<Table>& getViews() override {
        return views;
    }
    const std::vector<Table>& getViews() const override {
        return views;
    }

    const std::vector<std::string>& getSequences() const override {
        return sequences;
    }

    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset,
                                                       const std::string& whereClause = "",
                                                       const std::string& orderBy = "") override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName, const std::string& whereClause = "") override;

    [[nodiscard]] bool isTablesLoaded() const override {
        return tablesLoaded;
    }
    [[nodiscard]] bool isViewsLoaded() const override {
        return viewsLoaded;
    }
    [[nodiscard]] bool isLoadingTables() const override {
        return tablesLoader.isRunning();
    }
    [[nodiscard]] bool isLoadingViews() const override {
        return viewsLoader.isRunning();
    }

    void checkLoadingStatus() override;

    [[nodiscard]] const std::string& getLastTablesError() const override {
        return lastTablesError;
    }
    [[nodiscard]] const std::string& getLastViewsError() const override {
        return lastViewsError;
    }

    void startTableRefreshAsync(const std::string& tableName) override;
    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override;
    void checkTableRefreshStatusAsync(const std::string& tableName) override;

    void startTablesLoadAsync(bool force = false) override;
    void checkTablesStatusAsync();
    std::vector<Table> getTablesAsync();

    void startViewsLoadAsync(bool force = false) override;
    void checkViewsStatusAsync();
    std::vector<Table> getViewsWithColumnsAsync();

    void startMaterializedViewsLoadAsync(bool forceRefresh = false);
    void checkMaterializedViewsStatusAsync();
    std::vector<Table> getMaterializedViewsWithColumnsAsync();

    void startSequencesLoadAsync(bool forceRefresh = false);
    void checkSequencesStatusAsync();
    std::vector<std::string> getSequencesAsync();

    Table refreshTableAsync(const std::string& tableName);
};
