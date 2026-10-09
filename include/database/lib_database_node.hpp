#pragma once

#include "async_helper.hpp"
#include "connection_pool.hpp"
#include "database_node.hpp"
#include "table_data_provider.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

// a database (or schema) whose catalog and queries come from libdearsql. this
// keeps what the library leaves to its host: a pool of per-worker handles
// (cancellable), async loaders, and the caches the sidebar renders. backends
// say where a handle comes from and how the node is named.
class LibDatabaseNode : public IDatabaseNode, public ITableDataProvider {
public:
    template <typename T> using Loaded = LoadResult<T>;

    ~LibDatabaseNode() override;

    std::string name;

    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<Table> materializedViews;
    std::vector<std::string> sequences;
    std::vector<Routine> routines;

    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool materializedViewsLoaded = false;
    bool sequencesLoaded = false;
    bool routinesLoaded = false;

    AsyncOperation<Loaded<Table>> tablesLoader;
    AsyncOperation<Loaded<Table>> viewsLoader;
    AsyncOperation<Loaded<Table>> materializedViewsLoader;
    AsyncOperation<Loaded<std::string>> sequencesLoader;
    AsyncOperation<Loaded<Routine>> routinesLoader;
    std::map<std::string, AsyncOperation<Table>> tableRefreshLoaders;

    std::string lastTablesError;
    std::string lastViewsError;
    std::string lastMaterializedViewsError;
    std::string lastSequencesError;
    std::string lastRoutinesError;

    // sidebar expansion state
    bool expanded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;

    // ========== IDatabaseNode ==========

    [[nodiscard]] std::string getName() const override {
        return name;
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

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;
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
    [[nodiscard]] const std::string& getLastTablesError() const override {
        return lastTablesError;
    }
    [[nodiscard]] const std::string& getLastViewsError() const override {
        return lastViewsError;
    }

    void startTablesLoadAsync(bool force = false) override;
    void startViewsLoadAsync(bool force = false) override;
    void startMaterializedViewsLoadAsync(bool force = false);
    void startSequencesLoadAsync(bool force = false);
    void startRoutinesLoadAsync(bool force = false);
    // polls every loader; call once per frame
    void checkLoadingStatus() override;
    void checkTablesStatusAsync();
    void checkViewsStatusAsync();
    void checkMaterializedViewsStatusAsync();
    void checkSequencesStatusAsync();
    void checkRoutinesStatusAsync();

    void startTableRefreshAsync(const std::string& tableName) override;
    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override;
    void checkTableRefreshStatusAsync(const std::string& tableName) override;

    // ========== DDL (refreshes the affected list on success) ==========

    std::pair<bool, std::string> createTable(const Table& table) override;
    std::pair<bool, std::string> renameTable(const std::string& oldName,
                                             const std::string& newName);
    std::pair<bool, std::string> dropTable(const std::string& tableName);
    std::pair<bool, std::string> truncateTable(const std::string& tableName);
    std::pair<bool, std::string> dropColumn(const std::string& tableName,
                                            const std::string& columnName);
    std::pair<bool, std::string> dropView(const std::string& viewName, bool isMaterialized = false);

    [[nodiscard]] bool hasPendingAsyncWork() const;

    // run fn(dearsql::IDatabase&) on a pooled handle; throws when none can be opened
    template <typename F> auto withHandle(F&& fn) {
        using R = std::invoke_result_t<F, dearsql::IDatabase&>;
        if constexpr (std::is_void_v<R>) {
            run([&](dearsql::IDatabase& db) { fn(db); });
        } else {
            std::optional<R> out;
            run([&](dearsql::IDatabase& db) { out.emplace(fn(db)); });
            return std::move(*out);
        }
    }

    // restamped (nextGeneration) whenever a loaded list is replaced; parents
    // re-aggregate when the max over their children moves
    uint64_t generation = 0;
    static uint64_t nextGeneration();

    using Session = ConnectionPool<dearsql::DatabasePtr>::Session;
    // pin one pooled handle, for work that needs session state across statements
    Session acquire() {
        return pool().acquire();
    }

    // close the pooled handles (disconnect, refresh); the next call reopens them
    void resetPool();
    void cancelLoaders();
    // cancel and join every loader. concrete nodes call this from their own
    // destructor: a loader still running would otherwise reach a hook (run,
    // openHandle) of an object already half destroyed
    void waitForLoaders();

protected:
    // handles kept per node; 0 = one shared handle for backends that are already
    // thread-safe (a driver-side pool or an internal lock)
    [[nodiscard]] virtual size_t poolSize() const {
        return 2;
    }
    // where withHandle runs: a pooled (or shared) handle by default; schema nodes
    // run on their database node's handle instead
    virtual void run(const std::function<void(dearsql::IDatabase&)>& fn);
    // a fresh library handle for this node; null on failure
    virtual dearsql::DatabasePtr openHandle() = 0;
    // Table::fullName is "<prefix>.<table>"; the sidebar keys tabs on it
    [[nodiscard]] virtual std::string fullNamePrefix() const {
        return getFullPath();
    }
    void stampFullNames(std::vector<Table>& list) const;
    // run fn on a handle, catching its error into the result (worker-thread safe)
    template <typename T, typename F> Loaded<T> load(const char* what, F&& fn) {
        try {
            return {withHandle(fn), ""};
        } catch (const std::exception& e) {
            spdlog::error("{} ({}): {}", what, name, e.what());
            return {{}, e.what()};
        }
    }

private:
    using Pool = ConnectionPool<dearsql::DatabasePtr>;
    Pool& pool();
    dearsql::DatabasePtr sharedHandle();
    template <typename T>
    void apply(AsyncOperation<Loaded<T>>& loader, std::vector<T>& into, std::string& error,
               bool& loaded);

    std::unique_ptr<Pool> pool_;
    dearsql::DatabasePtr shared_;
    std::mutex poolMutex_;
};
