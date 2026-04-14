#pragma once

#include "database/async_helper.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <string>
#include <vector>

class MSSQLDatabaseNode;

class MSSQLSchemaNode : public IDatabaseNode, public ITableDataProvider {
public:
    MSSQLDatabaseNode* parentDbNode = nullptr;
    std::string name;

    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<Routine> routines;
    std::vector<std::string> sequences; // empty, for API compatibility

    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool routinesLoaded = false;

    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    AsyncOperation<std::vector<Routine>> routinesLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastRoutinesError;

    // IDatabaseNode
    [[nodiscard]] std::string getName() const override {
        return name;
    }
    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override;
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::MSSQL;
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

    void checkTablesStatusAsync();
    void checkViewsStatusAsync();
    void checkRoutinesStatusAsync();

    void startRoutinesLoadAsync(bool force = false);

    // schema modification
    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName);
    std::pair<bool, std::string> dropTable(const std::string& tableName);
    std::pair<bool, std::string> truncateTable(const std::string& tableName);
    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName);

private:
    std::vector<Table> getTablesAsync();
    std::vector<Table> getViewsAsync();
    std::vector<Routine> getRoutinesAsync();
    Table refreshTableAsync(const std::string& tableName);

    // build [schema].[table] qualified name
    std::string qualifyName(const std::string& tableName) const;
};
