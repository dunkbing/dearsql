#pragma once

#include "async_helper.hpp"
#include "database_node.hpp"
#include "db_interface.hpp"
#include "table_data_provider.hpp"
#include <map>
#include <sqlite3.h>

class SQLiteDatabase final : public IDatabaseNode,
                             public DatabaseInterface,
                             public ITableDataProvider {
public:
    SQLiteDatabase(const DatabaseConnectionInfo& connInfo);
    ~SQLiteDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    // Database info
    const std::string& getPath() const;

    bool areTablesLoaded() const {
        return tablesLoaded;
    }
    void setTablesLoaded(bool loaded) {
        tablesLoaded = loaded;
    }

    // ========== IDatabaseNode Implementation ==========

    [[nodiscard]] std::string getName() const override;
    [[nodiscard]] std::string getFullPath() const override;

    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::SQLITE;
    }

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override {
        return const_cast<SQLiteDatabase*>(this);
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

    // Overload without whereClause (internal use)
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset);
    // IDatabaseNode/ITableDataProvider implementation
    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
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

    // ========== Schema Modification ==========

    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName);
    std::pair<bool, std::string> dropTable(const std::string& tableName);
    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName);

    // ========== Internal Methods ==========

    std::vector<Table> getTablesAsync() const;
    std::vector<Table> getViewsAsync() const;

    // Session access
    sqlite3* getSession() const;

    // Async operation status
    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || tablesLoader.isRunning() || viewsLoader.isRunning();
    }

    // Async operations
    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    // Loading state
    bool tablesLoaded = false;
    bool viewsLoaded = false;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;

protected:
    std::vector<std::string> getTableNames() const;
    std::vector<Index> getTableIndexes(const std::string& tableName) const;
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName) const;

private:
    sqlite3* db_ = nullptr;

    std::vector<Table> tables;
    std::vector<Table> views;

    // Query execution
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query, int rowLimit = 1000);
};
