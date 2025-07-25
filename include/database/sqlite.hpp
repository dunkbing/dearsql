#pragma once

#include "db_interface.hpp"
#include <future>
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

class SQLiteDatabase final : public DatabaseInterface {
public:
    SQLiteDatabase(std::string name, std::string path);
    ~SQLiteDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool isConnecting() const override;
    void startConnectionAsync() override;
    void checkConnectionStatusAsync() override;

    // Database info
    const std::string &getName() const override;
    const std::string &getConnectionString() const override;
    const std::string &getPath() const override;
    void *getConnection() const override;
    DatabaseType getType() const override;

    // Table management
    void refreshTables() override;
    const std::vector<Table> &getTables() const override;
    std::vector<Table> &getTables() override;
    bool areTablesLoaded() const override;
    void setTablesLoaded(bool loaded) override;

    // View management
    void refreshViews() override;
    const std::vector<Table> &getViews() const override;
    std::vector<Table> &getViews() override;
    bool areViewsLoaded() const override;
    void setViewsLoaded(bool loaded) override;

    // Sequence management (not applicable for SQLite)
    void refreshSequences() override;
    const std::vector<std::string> &getSequences() const override;
    std::vector<std::string> &getSequences() override;
    bool areSequencesLoaded() const override;
    void setSequencesLoaded(bool loaded) override;

    // Query execution
    std::string executeQuery(const std::string &query) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string &query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string &tableName, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string &tableName) override;
    int getRowCount(const std::string &tableName) override;

    // Async table data loading
    void startTableDataLoadAsync(const std::string &tableName, int limit, int offset) override;
    bool isLoadingTableData() const override;
    void checkTableDataStatusAsync() override;
    bool hasTableDataResult() const override;
    std::vector<std::vector<std::string>> getTableDataResult() override;
    std::vector<std::string> getColumnNamesResult() override;
    int getRowCountResult() override;
    void clearTableDataResult() override;

    // UI state
    bool isExpanded() const override;
    void setExpanded(bool expanded) override;

    // Connection attempt tracking
    bool hasAttemptedConnection() const override;
    void setAttemptedConnection(bool attempted) override;
    const std::string &getLastConnectionError() const override;
    void setLastConnectionError(const std::string &error) override;

protected:
    std::vector<std::string> getTableNames() override;
    std::vector<Column> getTableColumns(const std::string &tableName) override;
    std::vector<std::string> getViewNames() override;
    std::vector<Column> getViewColumns(const std::string &viewName) override;
    std::vector<std::string> getSequenceNames() override;

private:
    std::string name;
    std::string path;
    std::unique_ptr<soci::session> session;
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences; // Empty for SQLite
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;
    bool attemptedConnection = false;
    std::string lastConnectionError;

    // Async table data loading state
    bool loadingTableData = false;
    bool hasTableDataReady = false;
    std::vector<std::vector<std::string>> tableDataResult;
    std::vector<std::string> columnNamesResult;
    int rowCountResult = 0;
    std::future<void> tableDataFuture;
};
