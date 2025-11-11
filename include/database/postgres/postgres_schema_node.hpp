#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "database/table_data_provider.hpp"
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
class PostgresSchemaNode : public ITableDataProvider {
public:
    PostgresDatabaseNode* parentDbNode = nullptr;
    std::string name;

    // Schema contents (only tables, views, sequences for now)
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;

    // Loading state flags
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;

    // Async operations
    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    AsyncOperation<std::vector<std::string>> sequencesLoader;

    // UI expansion state
    bool tablesExpanded = false;
    bool viewsExpanded = false;
    bool sequencesExpanded = false;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastSequencesError;

    // Methods
    void startTablesLoadAsync(bool forceRefresh = false);
    void checkTablesStatusAsync();
    std::vector<Table> getTablesWithColumnsAsync();

    void startViewsLoadAsync(bool forceRefresh = false);
    void checkViewsStatusAsync();
    std::vector<Table> getViewsWithColumnsAsync();

    void startSequencesLoadAsync(bool forceRefresh = false);
    void checkSequencesStatusAsync();
    std::vector<std::string> getSequencesAsync();

    // Query execution methods for TableViewerTab (ITableDataProvider interface)
    std::vector<std::vector<std::string>>
    getTableData(const std::string& tableName, int limit, int offset,
                 const std::string& whereClause = "") override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName, const std::string& whereClause = "") override;
    std::string executeQuery(const std::string& query) override;
    const std::vector<Table>& getTables() const override {
        return tables;
    }
    const std::vector<Table>& getViews() const override {
        return views;
    }
};
