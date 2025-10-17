#pragma once

#include "database/db.hpp"
#include "database/table_data_provider.hpp"
#include <atomic>
#include <future>
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

    // Methods
    void startTablesLoadAsync();
    void checkTablesStatusAsync();
    std::vector<Table> getTablesWithColumnsAsync();

    void startViewsLoadAsync();
    void checkViewsStatusAsync();
    std::vector<Table> getViewsWithColumnsAsync();

    void startSequencesLoadAsync();
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
