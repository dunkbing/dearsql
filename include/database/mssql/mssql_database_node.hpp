#pragma once

#include "database/async_helper.hpp"
#include "database/connection_pool.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "database/mssql/mssql_fwd.hpp"

class MSSQLDatabase;

class MSSQLDatabaseNode : public IDatabaseNode, public ITableDataProvider {
public:
    MSSQLDatabase* parentDb = nullptr;
    std::string name;

    std::unique_ptr<ConnectionPool<DBPROCESS*>> connectionPool;

    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences; // empty, for API compatibility

    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;

    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    std::string lastTablesError;
    std::string lastViewsError;

    bool expanded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;

    // IDatabaseNode
    [[nodiscard]] std::string getName() const override {
        return name;
    }
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

    // internal
    void ensureConnectionPool();
    ConnectionPool<DBPROCESS*>::Session getSession() const;
    void initializeConnectionPool(const DatabaseConnectionInfo& info);

    // schema modification
    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName);
    std::pair<bool, std::string> dropTable(const std::string& tableName);
    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName);

    void checkTablesStatusAsync();
    void checkViewsStatusAsync();

private:
    std::vector<Table> getTablesAsync();
    std::vector<Table> getViewsForDatabaseAsync();
    Table refreshTableAsync(const std::string& tableName);
};
