#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include <memory>
#include <soci/connection-pool.h>
#include <string>
#include <vector>

// Forward declaration
class MySQLDatabase;

/**
 * @brief Per-database data for MySQL
 *
 * MySQL hierarchy: Server → Databases → (app_db, reporting_db, ...) → Tables/Views
 * Each MySQLDatabaseNode represents one database within the MySQL server.
 * Note: MySQL doesn't have schemas, so tables/views are directly under database.
 */
class MySQLDatabaseNode : public ITableDataProvider {
public:
    MySQLDatabase* parentDb = nullptr;

    std::string name;

    // Connection pool (one per database)
    std::unique_ptr<soci::connection_pool> connectionPool;

    // MySQL: Database → Tables/Views (no schema layer)
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences; // Empty for MySQL (for API compatibility)

    // Loading state flags
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false; // For API compatibility

    // Async operations
    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;

    // UI expansion state
    bool expanded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;
    bool sequencesExpanded = false; // For API compatibility

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;

    // Methods
    void ensureConnectionPool();

    void startTablesLoadAsync(bool forceRefresh = false);
    void checkTablesStatusAsync();
    std::vector<Table> getTablesAsync();

    void startViewsLoadAsync(bool forceRefresh = false);
    void checkViewsStatusAsync();
    std::vector<Table> getViewsForDatabaseAsync();

    std::unique_ptr<soci::session> getSession() const;
    void initializeConnectionPool(const DatabaseConnectionInfo& info);

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
