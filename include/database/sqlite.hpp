#pragma once

#include "base_database.hpp"
#include "table_data_provider.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

class SQLiteDatabase final : public BaseDatabaseImpl, public ITableDataProvider {
public:
    SQLiteDatabase(const DatabaseConnectionInfo& connInfo);
    ~SQLiteDatabase() override;

    // Connection management (BaseDatabaseImpl handles async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    // Database info
    const std::string& getPath() const;
    void* getConnection() const override;

    bool areTablesLoaded() const {
        return tablesLoaded;
    }
    void setTablesLoaded(bool loaded) {
        tablesLoaded = loaded;
    }

    // Schema management (BaseDatabaseImpl provides getters/setters)
    void refreshAllTables();

    // Async table/view/sequence loading
    void startTablesLoadAsync(bool forceRefresh = false);
    void checkTablesStatusAsync();
    std::vector<Table> getTablesAsync() const;

    void startViewsLoadAsync(bool forceRefresh = false);
    void checkViewsStatusAsync();
    std::vector<Table> getViewsAsync() const;

    // Query execution
    std::string executeQuery(const std::string& query) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query);

    // DatabaseInterface implementation (without whereClause)
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset);
    std::vector<std::string> getColumnNames(const std::string& tableName) override;

    // ITableDataProvider implementation (with whereClause)
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset,
                                                       const std::string& whereClause) override;
    int getRowCount(const std::string& tableName, const std::string& whereClause = "") override;
    const std::vector<Table>& getTables() const override {
        return tables;
    }
    const std::vector<Table>& getViews() const override {
        return views;
    }

    // query execution with comprehensive result
    QueryResult executeQueryWithResult(const std::string& query, int rowLimit = 1000);

    // Session access (returns raw pointer since SQLite has single session)
    soci::session* getSession() const;

    // Loading state
    std::atomic<bool> loadingTables = false;
    std::atomic<bool> loadingViews = false;
    std::atomic<bool> loadingSequences = false;

    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastSequencesError;

protected:
    std::vector<std::string> getTableNames() const;
    std::vector<Index> getTableIndexes(const std::string& tableName) const;
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName) const;

private:
    // SQLite-specific state (base class handles common state)
    std::unique_ptr<soci::session> session;

    // Async futures
    std::future<std::vector<Table>> tablesFuture;
    std::future<std::vector<Table>> viewsFuture;
    std::future<std::vector<std::string>> sequencesFuture;
};
