#pragma once

#include "async_helper.hpp"
#include "connection_pool.hpp"
#include "database_node.hpp"
#include "table_data_provider.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

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
    // a requested reload counts as loading: the next status check starts it
    [[nodiscard]] bool isLoadingTables() const override {
        return tablesLoader.isRunning() || tablesReloadPending_;
    }
    [[nodiscard]] bool isLoadingViews() const override {
        return viewsLoader.isRunning() || viewsReloadPending_;
    }
    [[nodiscard]] bool isLoadingMaterializedViews() const {
        return materializedViewsLoader.isRunning() || materializedViewsReloadPending_;
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

    // any thread: the next status check (UI thread) reloads; a load already
    // running is followed by one more instead of the request being dropped
    void requestTablesReload() {
        tablesReloadPending_ = true;
    }
    void requestViewsReload() {
        viewsReloadPending_ = true;
    }
    void requestMaterializedViewsReload() {
        materializedViewsReloadPending_ = true;
    }
    void requestTableRefresh(const std::string& tableName);

    // ========== DDL (any thread; the affected list reloads on the next check) ==========

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
        const CallGuard guard(*this);
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

    // one pinned pooled handle; keeps its pool alive while held
    class Session {
    public:
        explicit Session(std::shared_ptr<ConnectionPool<dearsql::DatabasePtr>> pool)
            : pool_(std::move(pool)), session_(pool_->acquire()) {}
        [[nodiscard]] dearsql::DatabasePtr get() const {
            return session_.get();
        }

    private:
        std::shared_ptr<ConnectionPool<dearsql::DatabasePtr>> pool_; // outlives session_
        ConnectionPool<dearsql::DatabasePtr>::Session session_;
    };
    // pin one pooled handle, for work that needs session state across statements
    Session acquire() {
        return Session(pool());
    }

    // drop the pooled handles (disconnect, refresh); the next call reopens them.
    // busy ones are cancelled and the old pool closes on the reaper; `now` does
    // both here and waits for every session to come back (before DROP DATABASE)
    void resetPool(bool now = false);
    void cancelLoaders();
    // cancel the running loaders server-side too, then join them
    void stopLoaders();
    // stopLoaders, close the pool, and wait out calls other threads (tabs) are
    // still making. concrete nodes call this from their own destructor: a call
    // still running would otherwise reach a hook (run, openHandle) of an object
    // already half destroyed. it blocks: owners destroy nodes on the reaper
    void waitForLoaders();
    // stop running loaders without waiting (server-side cancel on the reaper)
    void abandonLoaders();
    // while suspended no handle is opened (calls fail fast): a DROP DATABASE on a
    // worker must not race a loader reopening a session on it
    void setSuspended(bool suspended);
    // keeps the node from finishing destruction while held: taken on the UI thread
    // for a worker that uses the node after the UI may have retired it
    [[nodiscard]] std::shared_ptr<void> pin() override;

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
    std::shared_ptr<Pool> pool();
    dearsql::DatabasePtr sharedHandle();
    template <typename T>
    void apply(AsyncOperation<Loaded<T>>& loader, std::vector<T>& into, std::string& error,
               bool& loaded);
    // worker ids of the running loaders
    std::vector<std::thread::id> runningLoaderIds() const;

    // calls in flight on any thread; the destructor waits them out
    struct CallGuard {
        explicit CallGuard(LibDatabaseNode& n) : node(n) {
            ++node.calls_;
        }
        ~CallGuard() {
            if (--node.calls_ == 0)
                node.calls_.notify_all();
        }
        LibDatabaseNode& node;
    };

    std::shared_ptr<Pool> pool_;
    dearsql::DatabasePtr shared_;
    std::mutex poolMutex_;
    bool closed_ = false;    // under poolMutex_; set by waitForLoaders, no new pool after
    bool suspended_ = false; // under poolMutex_
    std::atomic<int> calls_{0};

    std::atomic<bool> tablesReloadPending_{false};
    std::atomic<bool> viewsReloadPending_{false};
    std::atomic<bool> materializedViewsReloadPending_{false};
    mutable std::mutex refreshRequestMutex_;
    std::set<std::string> refreshRequests_; // under refreshRequestMutex_
    // starts what requestTableRefresh queued (UI thread)
    void startRequestedRefreshes();
};
