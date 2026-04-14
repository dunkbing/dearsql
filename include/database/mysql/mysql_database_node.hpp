#pragma once

#include "database/async_helper.hpp"
#include "database/connection_pool.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <memory>
#include <mysql/mysql.h>
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
class MySQLDatabaseNode : public IDatabaseNode, public ITableDataProvider {
public:
    MySQLDatabase* parentDb = nullptr;

    std::string name;

    // Connection pool (one per database)
    std::unique_ptr<ConnectionPool<MYSQL*>> connectionPool;

    // MySQL: Database → Tables/Views (no schema layer)
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences; // Empty for MySQL (for API compatibility)
    std::vector<Routine> routines;

    // Loading state flags
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false; // For API compatibility
    bool routinesLoaded = false;

    // Async operations
    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    AsyncOperation<std::vector<Routine>> routinesLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastRoutinesError;

    // UI expansion state (to be moved to ViewModel in later phase)
    bool expanded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;

    // ========== IDatabaseNode Implementation ==========

    [[nodiscard]] std::string getName() const override {
        return name;
    }

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;

    [[nodiscard]] std::string getFullPath() const override;

    [[nodiscard]] DatabaseType getDatabaseType() const override;

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

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause = "",
                                                       const std::string& orderBy = "") override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause = "") override;

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

    void startTablesLoadAsync(bool force = false) override;
    void startViewsLoadAsync(bool force = false) override;
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

    // ========== Internal Methods ==========

    void ensureConnectionPool();
    void checkTablesStatusAsync();
    std::vector<Table> getTablesAsync();
    void checkViewsStatusAsync();
    std::vector<Table> getViewsForDatabaseAsync();
    void startRoutinesLoadAsync(bool force = false);
    void checkRoutinesStatusAsync();
    std::vector<Routine> getRoutinesAsync();
    Table refreshTableAsync(const std::string& tableName);

    ConnectionPool<MYSQL*>::Session getSession() const;
    void initializeConnectionPool(const DatabaseConnectionInfo& info);

    // ========== Schema Modification ==========

    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName);
    std::pair<bool, std::string> dropTable(const std::string& tableName);
    std::pair<bool, std::string> truncateTable(const std::string& tableName);
    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName);
};
