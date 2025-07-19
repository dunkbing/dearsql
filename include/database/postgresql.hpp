#pragma once

#include "db_interface.hpp"
#include <atomic>
#include <future>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <thread>

class PostgreSQLDatabase : public DatabaseInterface {
public:
    PostgreSQLDatabase(const std::string &name, const std::string &host, int port,
                       const std::string &database, const std::string &username,
                       const std::string &password);
    ~PostgreSQLDatabase() override;

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

    // Sequence management
    void refreshSequences() override;
    const std::vector<std::string> &getSequences() const override;
    std::vector<std::string> &getSequences() override;
    bool areSequencesLoaded() const override;
    void setSequencesLoaded(bool loaded) override;
    bool isLoadingSequences() const override;
    void checkSequencesStatusAsync() override;

    // Query execution
    std::string executeQuery(const std::string &query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string &tableName, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string &tableName) override;
    int getRowCount(const std::string &tableName) override;

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

    // Async loading helpers
    void startAsyncTableRefresh();
    std::vector<Table> getTablesWithColumnsAsync();
    void startAsyncViewRefresh();
    std::vector<Table> getViewsWithColumnsAsync();
    void startAsyncSequenceRefresh();
    std::vector<std::string> getSequencesAsync();

private:
    std::string name;
    std::string host;
    int port;
    std::string database;
    std::string username;
    std::string password;
    std::string connectionString;
    std::unique_ptr<soci::session> session;
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;
    bool attemptedConnection = false;
    std::string lastConnectionError;

    // Async loading
    std::atomic<bool> loadingTables = false;
    std::thread tablesThread;
    std::future<std::vector<Table>> tablesFuture;

    std::atomic<bool> loadingViews = false;
    std::thread viewsThread;
    std::future<std::vector<Table>> viewsFuture;

    std::atomic<bool> loadingSequences = false;
    std::thread sequencesThread;
    std::future<std::vector<std::string>> sequencesFuture;

    // Async connection
    std::atomic<bool> connecting = false;
    std::thread connectionThread;
    std::future<std::pair<bool, std::string>> connectionFuture;
};
