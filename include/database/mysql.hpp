#pragma once

#include "db_interface.hpp"
#include <atomic>
#include <future>
#include <mutex>
#include <set>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>

class MySQLDatabase final : public DatabaseInterface {
public:
    MySQLDatabase(const std::string &name, const std::string &host, int port,
                  const std::string &database, const std::string &username,
                  const std::string &password, bool showAllDatabases = false);
    ~MySQLDatabase() override;

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
    const std::string &getDatabaseName() const;

    // Table management
    void refreshTables() override;
    const std::vector<Table> &getTables() const override;
    std::vector<Table> &getTables() override;
    bool areTablesLoaded() const override;
    void setTablesLoaded(bool loaded) override;
    bool isLoadingTables() const override;
    void checkTablesStatusAsync() override;

    // View management
    void refreshViews() override;
    const std::vector<Table> &getViews() const override;
    std::vector<Table> &getViews() override;
    bool areViewsLoaded() const override;
    void setViewsLoaded(bool loaded) override;
    bool isLoadingViews() const override;
    void checkViewsStatusAsync() override;

    // Sequence management (not applicable for MySQL)
    void refreshSequences() override;
    const std::vector<std::string> &getSequences() const override;
    std::vector<std::string> &getSequences() override;
    bool areSequencesLoaded() const override;
    void setSequencesLoaded(bool loaded) override;
    bool isLoadingSequences() const override;
    void checkSequencesStatusAsync() override;

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

    // Database list methods
    std::vector<std::string> getDatabaseNames();
    bool shouldShowAllDatabases() const {
        return showAllDatabases;
    }
    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    std::pair<bool, std::string> switchToDatabase(const std::string &targetDatabase);
    bool isDatabaseExpanded(const std::string &dbName) const;
    void setDatabaseExpanded(const std::string &dbName, bool expanded);

protected:
    std::vector<std::string> getTableNames() override;
    std::vector<Column> getTableColumns(const std::string &tableName) override;
    std::vector<std::string> getViewNames() override;
    std::vector<Column> getViewColumns(const std::string &viewName) override;
    std::vector<std::string> getSequenceNames() override;

    // Async loading helpers
    void startRefreshTableAsync();
    std::vector<Table> getTablesWithColumnsAsync();
    void startRefreshViewAsync();
    std::vector<Table> getViewsWithColumnsAsync();

private:
    std::string name;
    std::string host;
    int port;
    std::string database;
    std::string username;
    std::string password;
    std::string connectionString;
    bool showAllDatabases;
    std::unique_ptr<soci::session> session;
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences; // Empty for MySQL
    std::vector<std::string> availableDatabases;
    std::set<std::string> expandedDatabases; // Track which databases have been expanded
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;
    bool databasesLoaded = false;
    bool attemptedConnection = false;
    std::string lastConnectionError;

    // Async loading
    std::atomic<bool> loadingTables = false;
    std::future<std::vector<Table>> tablesFuture;

    std::atomic<bool> loadingViews = false;
    std::future<std::vector<Table>> viewsFuture;

    // Async connection
    std::atomic<bool> connecting = false;
    std::future<std::pair<bool, std::string>> connectionFuture;

    // Async table data loading
    std::atomic<bool> loadingTableData = false;
    std::atomic<bool> hasTableDataReady = false;
    std::vector<std::vector<std::string>> tableDataResult;
    std::vector<std::string> columnNamesResult;
    int rowCountResult = 0;
    std::future<void> tableDataFuture;

    // Thread synchronization
    mutable std::mutex sessionMutex;
};
