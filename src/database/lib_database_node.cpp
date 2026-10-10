#include "database/lib_database_node.hpp"
#include "utils/reaper.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ranges>

LibDatabaseNode::~LibDatabaseNode() {
    cancelLoaders();
}

// connects outside poolMutex_ (a slow server must not stall resetPool or other
// callers); a racer that loses just drops its own pool
std::shared_ptr<LibDatabaseNode::Pool> LibDatabaseNode::pool() {
    {
        std::lock_guard lock(poolMutex_);
        if (suspended_)
            throw dearsql::Error(name + " is busy (being dropped or renamed)");
        if (pool_)
            return pool_;
        if (closed_)
            throw dearsql::Error("connection to " + name + " closed");
    }
    auto fresh = std::make_shared<Pool>(
        [this] {
            auto handle = openHandle();
            if (!handle)
                throw dearsql::Error("could not open a connection to " + name);
            return handle;
        },
        nullptr, [](const dearsql::DatabasePtr& db) { return db->alive(); },
        [](const dearsql::DatabasePtr& db) { db->cancel(); }, poolSize());
    std::lock_guard lock(poolMutex_);
    // torn down while we connected: a pool installed now would run uncancelled
    if (closed_ || suspended_)
        throw dearsql::Error("connection to " + name + " closed");
    if (!pool_)
        pool_ = std::move(fresh);
    return pool_;
}

dearsql::DatabasePtr LibDatabaseNode::sharedHandle() {
    {
        std::lock_guard lock(poolMutex_);
        if (suspended_)
            throw dearsql::Error(name + " is busy (being dropped or renamed)");
        if (shared_)
            return shared_;
        if (closed_)
            throw dearsql::Error("connection to " + name + " closed");
    }
    auto fresh = openHandle();
    if (!fresh)
        throw dearsql::Error("could not open a connection to " + name);
    std::lock_guard lock(poolMutex_);
    if (closed_ || suspended_)
        throw dearsql::Error("connection to " + name + " closed");
    if (!shared_)
        shared_ = std::move(fresh);
    return shared_;
}

void LibDatabaseNode::run(const std::function<void(dearsql::IDatabase&)>& fn) {
    if (poolSize() == 0) {
        auto handle = sharedHandle();
        fn(*handle);
        return;
    }
    // our own reference: a resetPool meanwhile cannot free it under us
    auto p = pool();
    auto session = p->acquire();
    fn(*session.get());
}

void LibDatabaseNode::resetPool(bool now) {
    std::shared_ptr<Pool> old;
    dearsql::DatabasePtr shared;
    {
        std::lock_guard lock(poolMutex_);
        old = std::move(pool_);
        shared = std::move(shared_);
    }
    ConnectionPoolBase::Cancel cancel = old ? old->shutdown() : nullptr;
    if (now) {
        if (cancel)
            cancel();
        if (old)
            old->drain();
        return;
    }
    // a cancel and a close are network calls: off the calling (UI) thread
    Reaper::post(std::move(cancel));
    if (old)
        Reaper::dispose(std::move(old));
    if (shared)
        Reaper::dispose(std::move(shared));
}

void LibDatabaseNode::cancelLoaders() {
    tablesLoader.cancel();
    viewsLoader.cancel();
    materializedViewsLoader.cancel();
    sequencesLoader.cancel();
    routinesLoader.cancel();
    for (auto& loader : tableRefreshLoaders | std::views::values)
        loader.cancel();
}

void LibDatabaseNode::stampFullNames(std::vector<Table>& list) const {
    const std::string prefix = fullNamePrefix() + ".";
    for (auto& t : list)
        t.fullName = prefix + t.name;
}

QueryResult LibDatabaseNode::executeQuery(const std::string& sql, int limit) {
    const auto start = std::chrono::steady_clock::now();
    try {
        return withHandle([&](dearsql::IDatabase& db) {
            const double connectMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            auto result = db.execute(sql, limit);
            // pool checkout is the host's share of the waterfall
            if (!result.phaseTimings.empty())
                result.phaseTimings.insert(result.phaseTimings.begin(), {"Connect", connectMs});
            return result;
        });
    } catch (const std::exception& e) {
        QueryResult result;
        result.statements.push_back({.success = false, .errorMessage = e.what()});
        return result;
    }
}

std::vector<std::vector<std::string>> LibDatabaseNode::getTableData(const Table& table, int limit,
                                                                    int offset,
                                                                    const std::string& whereClause,
                                                                    const std::string& orderBy) {
    return load<std::vector<std::string>>("getTableData",
                                          [&](dearsql::IDatabase& db) {
                                              return db.getTableData(table, limit, offset,
                                                                     whereClause, orderBy);
                                          })
        .items;
}

std::vector<std::string> LibDatabaseNode::getColumnNames(const Table& table) {
    return load<std::string>("getColumnNames",
                             [&](dearsql::IDatabase& db) { return db.getColumnNames(table); })
        .items;
}

int LibDatabaseNode::getRowCount(const Table& table, const std::string& whereClause) {
    std::string err;
    return libCall(err, "getRowCount", [&] {
        return withHandle(
            [&](dearsql::IDatabase& db) { return db.getRowCount(table, whereClause); });
    });
}

std::pair<bool, std::string> LibDatabaseNode::getTableDdl(const Table& table) {
    std::string err;
    auto ddl = libCall(err, "table ddl", [&] {
        return withHandle([&](dearsql::IDatabase& db) { return db.tableDdl(table.name); });
    });
    return err.empty() ? std::pair{true, std::move(ddl)} : std::pair{false, std::move(err)};
}

// one stamp per change across every node: a parent comparing the max over its
// children can't be fooled by one going away while another moves on
uint64_t LibDatabaseNode::nextGeneration() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

void LibDatabaseNode::startTablesLoadAsync(bool force) {
    // a forced reload during a load runs once more after it (a DDL that landed
    // mid-load would otherwise leave the stale list)
    if (tablesLoader.isRunning()) {
        if (force)
            tablesReloadPending_ = true;
        return;
    }
    if (tablesLoaded && !force && !tablesReloadPending_)
        return;
    tablesReloadPending_ = false;
    tablesLoader.start([this] {
        auto r = load<Table>("load tables", [](dearsql::IDatabase& db) { return db.tables(); });
        stampFullNames(r.items);
        return r;
    });
}

void LibDatabaseNode::startViewsLoadAsync(bool force) {
    if (viewsLoader.isRunning()) {
        if (force)
            viewsReloadPending_ = true;
        return;
    }
    if (viewsLoaded && !force && !viewsReloadPending_)
        return;
    viewsReloadPending_ = false;
    viewsLoader.start([this] {
        auto r = load<Table>("load views", [](dearsql::IDatabase& db) { return db.views(); });
        stampFullNames(r.items);
        return r;
    });
}

void LibDatabaseNode::startMaterializedViewsLoadAsync(bool force) {
    if (materializedViewsLoader.isRunning()) {
        if (force)
            materializedViewsReloadPending_ = true;
        return;
    }
    if (materializedViewsLoaded && !force && !materializedViewsReloadPending_)
        return;
    materializedViewsReloadPending_ = false;
    materializedViewsLoader.start([this] {
        auto r = load<Table>("load materialized views",
                             [](dearsql::IDatabase& db) { return db.materializedViews(); });
        stampFullNames(r.items);
        return r;
    });
}

void LibDatabaseNode::startSequencesLoadAsync(bool force) {
    if (sequencesLoader.isRunning() || (sequencesLoaded && !force))
        return;
    sequencesLoader.start([this] {
        return load<std::string>("load sequences",
                                 [](dearsql::IDatabase& db) { return db.sequences(); });
    });
}

void LibDatabaseNode::startRoutinesLoadAsync(bool force) {
    if (routinesLoader.isRunning() || (routinesLoaded && !force))
        return;
    routinesLoader.start([this] {
        return load<Routine>("load routines", [](dearsql::IDatabase& db) { return db.routines(); });
    });
}

// results and errors land here, on the UI thread
template <typename T>
void LibDatabaseNode::apply(AsyncOperation<Loaded<T>>& loader, std::vector<T>& into,
                            std::string& error, bool& loaded) {
    loader.check([&](Loaded<T> r) {
        into = std::move(r.items);
        error = std::move(r.error);
        loaded = true;
        generation = nextGeneration();
    });
}

void LibDatabaseNode::checkTablesStatusAsync() {
    apply(tablesLoader, tables, lastTablesError, tablesLoaded);
    if (tablesReloadPending_ && !tablesLoader.isRunning())
        startTablesLoadAsync(true);
    startRequestedRefreshes();
}

void LibDatabaseNode::checkViewsStatusAsync() {
    apply(viewsLoader, views, lastViewsError, viewsLoaded);
    if (viewsReloadPending_ && !viewsLoader.isRunning())
        startViewsLoadAsync(true);
}

void LibDatabaseNode::checkMaterializedViewsStatusAsync() {
    apply(materializedViewsLoader, materializedViews, lastMaterializedViewsError,
          materializedViewsLoaded);
    if (materializedViewsReloadPending_ && !materializedViewsLoader.isRunning())
        startMaterializedViewsLoadAsync(true);
}

void LibDatabaseNode::checkSequencesStatusAsync() {
    apply(sequencesLoader, sequences, lastSequencesError, sequencesLoaded);
}

void LibDatabaseNode::checkRoutinesStatusAsync() {
    apply(routinesLoader, routines, lastRoutinesError, routinesLoaded);
}

void LibDatabaseNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
    checkMaterializedViewsStatusAsync();
    checkSequencesStatusAsync();
    checkRoutinesStatusAsync();
    std::vector<std::string> names;
    for (const auto& name : tableRefreshLoaders | std::views::keys)
        names.push_back(name);
    for (const auto& name : names)
        checkTableRefreshStatusAsync(name);
}

void LibDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    auto& loader = tableRefreshLoaders[tableName];
    if (loader.isRunning())
        return;
    loader.start([this, tableName] {
        std::string err;
        Table t = libCall(err, "describe table", [&] {
            return withHandle([&](dearsql::IDatabase& db) { return db.describeTable(tableName); });
        });
        t.fullName = fullNamePrefix() + "." + tableName;
        return t;
    });
}

void LibDatabaseNode::requestTableRefresh(const std::string& tableName) {
    std::lock_guard lock(refreshRequestMutex_);
    refreshRequests_.insert(tableName);
}

void LibDatabaseNode::startRequestedRefreshes() {
    std::set<std::string> requested;
    {
        std::lock_guard lock(refreshRequestMutex_);
        requested.swap(refreshRequests_);
    }
    for (const auto& name : requested)
        startTableRefreshAsync(name);
}

bool LibDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    {
        std::lock_guard lock(refreshRequestMutex_);
        if (refreshRequests_.contains(tableName))
            return true;
    }
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

namespace {
    template <typename F> std::pair<bool, std::string> ddl(LibDatabaseNode& node, F&& fn) {
        try {
            return node.withHandle(fn);
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    }
} // namespace

// the DDL calls run on any thread: they only request the reload
std::pair<bool, std::string> LibDatabaseNode::createTable(const Table& table) {
    auto r = ddl(*this, [&](dearsql::IDatabase& db) { return db.createTable(table); });
    if (r.first)
        requestTablesReload();
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::renameTable(const std::string& oldName,
                                                          const std::string& newName) {
    auto r = ddl(*this, [&](dearsql::IDatabase& db) { return db.renameTable(oldName, newName); });
    if (r.first)
        requestTablesReload();
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::dropTable(const std::string& tableName) {
    auto r = ddl(*this, [&](dearsql::IDatabase& db) { return db.dropTable(tableName); });
    if (r.first)
        requestTablesReload();
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::truncateTable(const std::string& tableName) {
    return ddl(*this, [&](dearsql::IDatabase& db) { return db.truncateTable(tableName); });
}

std::pair<bool, std::string> LibDatabaseNode::dropColumn(const std::string& tableName,
                                                         const std::string& columnName) {
    auto r =
        ddl(*this, [&](dearsql::IDatabase& db) { return db.dropColumn(tableName, columnName); });
    if (r.first)
        requestTableRefresh(tableName);
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::dropView(const std::string& viewName,
                                                       bool isMaterialized) {
    auto r =
        ddl(*this, [&](dearsql::IDatabase& db) { return db.dropView(viewName, isMaterialized); });
    if (r.first) {
        if (isMaterialized)
            requestMaterializedViewsReload();
        else
            requestViewsReload();
    }
    return r;
}

bool LibDatabaseNode::hasPendingAsyncWork() const {
    return tablesLoader.isRunning() || viewsLoader.isRunning() ||
           materializedViewsLoader.isRunning() || sequencesLoader.isRunning() ||
           routinesLoader.isRunning();
}

void LibDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    startRequestedRefreshes();
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end())
        return;
    it->second.check([&](Table t) {
        auto found = std::ranges::find(tables, tableName, &Table::name);
        if (found != tables.end() && !t.columns.empty()) {
            *found = std::move(t);
            generation = nextGeneration();
        }
    });
    // a cancelled one may still be in its query: erasing would join it here
    if (!it->second.isRunning() && !it->second.hasLiveWorker())
        tableRefreshLoaders.erase(it);
}

std::vector<std::thread::id> LibDatabaseNode::runningLoaderIds() const {
    std::vector<std::thread::id> ids;
    auto add = [&](const auto& loader) {
        if (loader.isRunning())
            ids.push_back(loader.workerId());
    };
    add(tablesLoader);
    add(viewsLoader);
    add(materializedViewsLoader);
    add(sequencesLoader);
    add(routinesLoader);
    for (const auto& loader : tableRefreshLoaders | std::views::values)
        add(loader);
    return ids;
}

void LibDatabaseNode::stopLoaders() {
    // the stop token never reaches a query blocked in the driver: the server
    // has to give it up. here, not on the reaper: we wait for it right after
    for (auto id : runningLoaderIds()) {
        for (auto& cancel : ConnectionPoolBase::cancelsFor(id))
            cancel();
    }
    cancelLoaders();
    tablesLoader.wait();
    viewsLoader.wait();
    materializedViewsLoader.wait();
    sequencesLoader.wait();
    routinesLoader.wait();
    for (auto& loader : tableRefreshLoaders | std::views::values)
        loader.wait();
}

void LibDatabaseNode::abandonLoaders() {
    for (auto id : runningLoaderIds())
        ConnectionPoolBase::cancelQueriesOn(id);
    cancelLoaders();
}

void LibDatabaseNode::waitForLoaders() {
    {
        std::lock_guard lock(poolMutex_);
        closed_ = true; // for good: only the destructor gets here
    }
    stopLoaders();
    resetPool(true);
    // a tab's detached worker may still be inside a call on us
    for (int n = calls_.load(); n != 0; n = calls_.load())
        calls_.wait(n);
}

void LibDatabaseNode::setSuspended(bool suspended) {
    std::lock_guard lock(poolMutex_);
    suspended_ = suspended;
}

std::shared_ptr<void> LibDatabaseNode::pin() {
    ++calls_;
    return {nullptr, [this](void*) {
                if (--calls_ == 0)
                    calls_.notify_all();
            }};
}
