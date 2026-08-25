#pragma once

#include "async_helper.hpp"
#include "database_node.hpp"
#include "db_interface.hpp"
#include "sql_builder.hpp"
#include "table_data_provider.hpp"
#include <algorithm>
#include <map>

// shared base for single-file backends (SQLite, DuckDB): connection + node in one
// class. subclasses supply the engine specifics (connect/disconnect, query
// execution, get*Async metadata fetchers); the async plumbing, schema caches and
// builder-based schema modification live here.
class FileDatabase : public IDatabaseNode, public DatabaseInterface, public ITableDataProvider {
public:
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
    [[nodiscard]] bool isLoadingTables() const override {
        return tablesLoader.isRunning();
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
        sequencesLoader.check([this](std::vector<std::string> result) {
            sequences = std::move(result);
            sequencesLoaded = true;
            spdlog::debug("Sequence loading completed. Found {} sequences", sequences.size());
        });
    }

    void checkLoadingStatus() override {
        tablesLoader.check([this](std::vector<Table> result) {
            tables = std::move(result);
            tablesLoaded = true;
            spdlog::debug("Table loading completed. Found {} tables", tables.size());
        });
        viewsLoader.check([this](std::vector<Table> result) {
            views = std::move(result);
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

    // ========== Schema Modification ==========

    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName) {
        const auto builder = createSQLBuilder(getDatabaseType());
        auto r = executeQuery(builder->renameTable("", oldName, newName));
        if (r.success()) {
            startTablesLoadAsync(true);
            return {true, ""};
        }
        return {false, r.errorMessage()};
    }

    std::pair<bool, std::string> dropTable(const std::string& tableName) {
        const auto builder = createSQLBuilder(getDatabaseType());
        auto r = executeQuery(builder->dropTable("", tableName));
        if (r.success()) {
            startTablesLoadAsync(true);
            return {true, ""};
        }
        return {false, r.errorMessage()};
    }

    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName) {
        const auto builder = createSQLBuilder(getDatabaseType());
        auto r = executeQuery(builder->dropColumn(builder->quoteIdentifier(tableName), columnName));
        if (r.success()) {
            startTablesLoadAsync(true);
            return {true, ""};
        }
        return {false, r.errorMessage()};
    }

    // ========== Internal Methods ==========

    virtual std::vector<Table> getTablesAsync() const = 0;
    virtual std::vector<Table> getViewsAsync() const = 0;
    virtual std::vector<std::string> getSequencesAsync() const = 0;

    // Async operation status
    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || tablesLoader.isRunning() || viewsLoader.isRunning() ||
               sequencesLoader.isRunning();
    }

    // Async operations
    AsyncOperation<std::vector<Table>> tablesLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    AsyncOperation<std::vector<std::string>> sequencesLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    // Loading state
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;

    // Error tracking
    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastSequencesError;

protected:
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;
};
