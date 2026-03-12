#pragma once

#include "async_helper.hpp"
#include "database_node.hpp"
#include "db_interface.hpp"
#include "table_data_provider.hpp"
#include <duckdb.h>
#include <map>

class DuckDBDatabase final : public IDatabaseNode,
                             public DatabaseInterface,
                             public ITableDataProvider {
public:
    explicit DuckDBDatabase(const DatabaseConnectionInfo& connInfo);
    ~DuckDBDatabase() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    [[nodiscard]] std::string getName() const override;
    [[nodiscard]] std::string getFullPath() const override;
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::DUCKDB;
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

    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset) {
        return getTableData(tableName, limit, offset, "");
    }
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset, const std::string& whereClause,
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
    void checkTableRefreshStatusAsync(const std::string&) override {}

    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || tablesLoader.isRunning() || viewsLoader.isRunning();
    }

private:
    std::vector<Table> getTablesAsync() const;
    std::vector<Table> getViewsAsync() const;
    Table loadTableMetadata(const std::string& tableName) const;

    duckdb_database db_ = nullptr;
    duckdb_connection conn_ = nullptr;

    std::vector<Table> tables;
    std::vector<Table> views;

    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    bool tablesLoaded = false;
    bool viewsLoaded = false;
    std::string lastTablesError;
    std::string lastViewsError;
};
