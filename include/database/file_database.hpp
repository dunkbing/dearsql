#pragma once

#include "async_helper.hpp"
#include "database_node.hpp"
#include "db_interface.hpp"
#include "sql_builder.hpp"
#include "table_data_provider.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <ranges>

// single-file backends (SQLite, DuckDB, CSV via DuckDB): connection + node in one
// class. libdearsql does the database work; this keeps the async loaders, the
// schema caches the sidebar renders, and the read-only guard.
class FileDatabase final : public IDatabaseNode,
                           public DatabaseInterface,
                           public ITableDataProvider {
public:
    explicit FileDatabase(const DatabaseConnectionInfo& info) {
        connectionInfo = info;
    }
    ~FileDatabase() override {
        // join every worker while the members they touch still exist
        connectionOp.wait();
        tablesLoader.wait();
        viewsLoader.wait();
        sequencesLoader.wait();
        for (auto& loader : tableRefreshLoaders | std::views::values)
            loader.wait();
        FileDatabase::disconnect();
    }

    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return connectionInfo.type;
    }

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;
    std::pair<bool, std::string> createTable(const Table& table) override;
    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderBy = "") override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause = "") override;
    void startTableRefreshAsync(const std::string& tableName) override;

    const std::string& getPath() const {
        return connectionInfo.path;
    }

    bool areTablesLoaded() const {
        return tablesLoaded;
    }
    void setTablesLoaded(bool loaded) {
        tablesLoaded = loaded;
    }

    // ========== IDatabaseNode Implementation ==========

    [[nodiscard]] std::string getName() const override {
        const auto& path = connectionInfo.path;
        auto pos = path.find_last_of("/\\");
        if (pos != std::string::npos) {
            return path.substr(pos + 1);
        }
        return path;
    }

    [[nodiscard]] std::string getFullPath() const override {
        return connectionInfo.path;
    }

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override {
        return const_cast<FileDatabase*>(this);
    }

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

    const std::vector<std::string>& getSequences() const override {
        return sequences;
    }

    [[nodiscard]] bool isTablesLoaded() const override {
        return tablesLoaded;
    }
    [[nodiscard]] bool isViewsLoaded() const override {
        return viewsLoaded;
    }
    // a reload a DDL asked for counts: checkLoadingStatus starts it
    [[nodiscard]] bool isLoadingTables() const override {
        return tablesLoader.isRunning() || tablesReloadPending_;
    }
    [[nodiscard]] bool isLoadingViews() const override {
        return viewsLoader.isRunning();
    }

    [[nodiscard]] const std::string& getLastTablesError() const override {
        return lastTablesError;
    }
    [[nodiscard]] const std::string& getLastViewsError() const override {
        return lastViewsError;
    }

    void startTablesLoadAsync(bool forceRefresh = false) override {
        spdlog::debug("startTablesLoadAsync for file database{}",
                      (forceRefresh ? " (force refresh)" : ""));

        // a forced reload during a load runs once more after it
        if (tablesLoader.isRunning()) {
            if (forceRefresh)
                tablesReloadPending_ = true;
            return;
        }
        if (tablesReloadPending_.exchange(false))
            forceRefresh = true;

        if (forceRefresh) {
            tables.clear();
            tablesLoaded = false;
            lastTablesError.clear();
        }

        if (!forceRefresh && tablesLoaded) {
            return;
        }

        tables.clear();
        tablesLoader.start([this]() { return getTablesAsync(); });
    }

    void startViewsLoadAsync(bool forceRefresh = false) override {
        spdlog::debug("startViewsLoadAsync for file database{}",
                      (forceRefresh ? " (force refresh)" : ""));

        if (forceRefresh) {
            views.clear();
            viewsLoaded = false;
            lastViewsError.clear();
        }

        if (!forceRefresh && viewsLoaded) {
            return;
        }

        views.clear();
        viewsLoader.start([this]() { return getViewsAsync(); });
    }

    void startSequencesLoadAsync(bool forceRefresh = false) {
        spdlog::debug("startSequencesLoadAsync for file database{}",
                      (forceRefresh ? " (force refresh)" : ""));

        if (sequencesLoader.isRunning()) {
            return;
        }

        if (forceRefresh) {
            sequences.clear();
            sequencesLoaded = false;
            lastSequencesError.clear();
        }

        if (!forceRefresh && sequencesLoaded) {
            return;
        }

        sequences.clear();
        sequencesLoader.start([this]() { return getSequencesAsync(); });
    }

    void checkSequencesStatusAsync() {
        sequencesLoader.check([this](LoadResult<std::string> result) {
            sequences = std::move(result.items);
            lastSequencesError = std::move(result.error);
            sequencesLoaded = true;
            spdlog::debug("Sequence loading completed. Found {} sequences", sequences.size());
        });
    }

    void checkLoadingStatus() override {
        tablesLoader.check([this](LoadResult<Table> result) {
            tables = std::move(result.items);
            lastTablesError = std::move(result.error);
            tablesLoaded = true;
            spdlog::debug("Table loading completed. Found {} tables", tables.size());
        });
        if (tablesReloadPending_ && !tablesLoader.isRunning())
            startTablesLoadAsync(true);
        viewsLoader.check([this](LoadResult<Table> result) {
            views = std::move(result.items);
            lastViewsError = std::move(result.error);
            viewsLoaded = true;
            spdlog::debug("View loading completed. Found {} views", views.size());
        });
        checkSequencesStatusAsync();
        for (auto it = tableRefreshLoaders.begin(); it != tableRefreshLoaders.end();) {
            const auto& tableName = it->first;
            it->second.check([this, &tableName](Table refreshedTable) {
                auto tableIt =
                    std::find_if(tables.begin(), tables.end(),
                                 [&tableName](const Table& t) { return t.name == tableName; });
                if (tableIt != tables.end()) {
                    *tableIt = std::move(refreshedTable);
                    spdlog::debug("Table {} refreshed successfully", tableName);
                }
            });
            if (!it->second.isRunning()) {
                it = tableRefreshLoaders.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override {
        auto it = tableRefreshLoaders.find(tableName);
        return it != tableRefreshLoaders.end() && it->second.isRunning();
    }

    void checkTableRefreshStatusAsync(const std::string& tableName) override {
        // handled by checkLoadingStatus
    }

    [[nodiscard]] std::shared_ptr<dearsql::IConnection> libConnection() const override {
        std::lock_guard lock(handleMutex_);
        return conn_;
    }

    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName);
    std::pair<bool, std::string> dropTable(const std::string& tableName);
    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName);

    LoadResult<Table> getTablesAsync();
    LoadResult<Table> getViewsAsync();
    LoadResult<std::string> getSequencesAsync();

    // Async operation status
    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || tablesLoader.isRunning() || viewsLoader.isRunning() ||
               sequencesLoader.isRunning();
    }

    // Async operations
    AsyncOperation<LoadResult<Table>> tablesLoader;
    AsyncOperation<LoadResult<Table>> viewsLoader;
    AsyncOperation<LoadResult<std::string>> sequencesLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    // Loading state
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastSequencesError;

private:
    // the shared library handle; loaders copy the pointer so disconnect() mid-load is safe
    [[nodiscard]] dearsql::DatabasePtr handle() const {
        std::lock_guard lock(handleMutex_);
        return db_;
    }
    std::pair<bool, std::string> afterDdl(const dearsql::Status& status);

    std::shared_ptr<dearsql::IConnection> conn_;
    dearsql::DatabasePtr db_;
    mutable std::mutex handleMutex_;
    std::atomic<bool> tablesReloadPending_{false}; // set by DDL on any thread

protected:
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;
};
