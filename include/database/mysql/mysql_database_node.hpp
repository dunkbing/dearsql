#pragma once

#include "database/db.hpp"
#include <atomic>
#include <future>
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
class MySQLDatabaseNode {
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
    std::atomic<bool> loadingTables = false;
    std::atomic<bool> loadingViews = false;

    // Async futures
    std::future<std::vector<Table>> tablesFuture;
    std::future<std::vector<Table>> viewsFuture;

    // UI expansion state
    bool expanded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;
    bool sequencesExpanded = false; // For API compatibility

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;

    // Methods
    void startTablesLoadAsync();
    void checkTablesStatusAsync();
    std::vector<Table> getTablesForDatabaseAsync();

    void startViewsLoadAsync();
    void checkViewsStatusAsync();
    std::vector<Table> getViewsForDatabaseAsync();

    std::unique_ptr<soci::session> getSession() const;
    void initializeConnectionPool(const std::string& connStr);

    // Table data operations (for table viewer)
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset,
                                                       const std::string& whereClause = "");
    std::vector<std::string> getColumnNames(const std::string& tableName);
    int getRowCount(const std::string& tableName, const std::string& whereClause = "");
    std::string executeQuery(const std::string& query);
};
