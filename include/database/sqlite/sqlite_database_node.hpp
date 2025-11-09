#pragma once

#include "database/db.hpp"
#include "database/table_data_provider.hpp"
#include <atomic>
#include <future>
#include <soci/soci.h>
#include <string>
#include <vector>

// Forward declaration
class SQLiteDatabase;

/**
 * @brief Per-database data for SQLite
 *
 * SQLite hierarchy: File → Tables/Views
 * SQLite is a single-file database, so there's no "databases" layer.
 * This node holds the contents of the single .db file.
 */
class SQLiteDatabaseNode : public ITableDataProvider {
public:
    SQLiteDatabase* parentDb = nullptr;

    std::string name; // File name/path

    // SQLite: Direct Tables/Views (no database or schema layers)
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences; // SQLite has sqlite_sequence

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
    bool expanded = false;
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
    std::vector<Table> getTablesAsync();

    void startViewsLoadAsync();
    void checkViewsStatusAsync();
    std::vector<Table> getViewsAsync();

    void startSequencesLoadAsync();
    void checkSequencesStatusAsync();
    std::vector<std::string> getSequencesAsync();

    // Table data operations (for table viewer - ITableDataProvider interface)
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

    // query execution with comprehensive result
    QueryResult executeQueryWithResult(const std::string& query, int rowLimit = 1000);
};
