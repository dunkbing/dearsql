#include "database/lib_database_node.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ranges>

LibDatabaseNode::~LibDatabaseNode() {
    cancelLoaders();
}

LibDatabaseNode::Pool& LibDatabaseNode::pool() {
    std::lock_guard lock(poolMutex_);
    if (!pool_) {
        pool_ = std::make_unique<Pool>(
            [this] {
                auto handle = openHandle();
                if (!handle)
                    throw dearsql::Error("could not open a connection to " + name);
                return handle;
            },
            nullptr, [](const dearsql::DatabasePtr& db) { return db->alive(); },
            [](const dearsql::DatabasePtr& db) { db->cancel(); }, poolSize());
    }
    return *pool_;
}

dearsql::DatabasePtr LibDatabaseNode::sharedHandle() {
    std::lock_guard lock(poolMutex_);
    if (!shared_)
        shared_ = openHandle();
    if (!shared_)
        throw dearsql::Error("could not open a connection to " + name);
    return shared_;
}

void LibDatabaseNode::run(const std::function<void(dearsql::IDatabase&)>& fn) {
    if (poolSize() == 0) {
        fn(*sharedHandle());
        return;
    }
    auto session = pool().acquire();
    fn(*session.get());
}

void LibDatabaseNode::resetPool() {
    std::lock_guard lock(poolMutex_);
    pool_.reset();
    shared_.reset();
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

// one stamp per change across every node: a parent comparing the max over its
// children can't be fooled by one going away while another moves on
uint64_t LibDatabaseNode::nextGeneration() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

void LibDatabaseNode::startTablesLoadAsync(bool force) {
    if (tablesLoader.isRunning() || (tablesLoaded && !force))
        return;
    tablesLoader.start([this] {
        auto r = load<Table>("load tables", [](dearsql::IDatabase& db) { return db.tables(); });
        stampFullNames(r.items);
        return r;
    });
}

void LibDatabaseNode::startViewsLoadAsync(bool force) {
    if (viewsLoader.isRunning() || (viewsLoaded && !force))
        return;
    viewsLoader.start([this] {
        auto r = load<Table>("load views", [](dearsql::IDatabase& db) { return db.views(); });
        stampFullNames(r.items);
        return r;
    });
}

void LibDatabaseNode::startMaterializedViewsLoadAsync(bool force) {
    if (materializedViewsLoader.isRunning() || (materializedViewsLoaded && !force))
        return;
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
}

void LibDatabaseNode::checkViewsStatusAsync() {
    apply(viewsLoader, views, lastViewsError, viewsLoaded);
}

void LibDatabaseNode::checkMaterializedViewsStatusAsync() {
    apply(materializedViewsLoader, materializedViews, lastMaterializedViewsError,
          materializedViewsLoaded);
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

bool LibDatabaseNode::isTableRefreshing(const std::string& tableName) const {
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

std::pair<bool, std::string> LibDatabaseNode::createTable(const Table& table) {
    auto r = ddl(*this, [&](dearsql::IDatabase& db) { return db.createTable(table); });
    if (r.first)
        startTablesLoadAsync(true);
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::renameTable(const std::string& oldName,
                                                          const std::string& newName) {
    auto r = ddl(*this, [&](dearsql::IDatabase& db) { return db.renameTable(oldName, newName); });
    if (r.first)
        startTablesLoadAsync(true);
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::dropTable(const std::string& tableName) {
    auto r = ddl(*this, [&](dearsql::IDatabase& db) { return db.dropTable(tableName); });
    if (r.first)
        startTablesLoadAsync(true);
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
        startTableRefreshAsync(tableName);
    return r;
}

std::pair<bool, std::string> LibDatabaseNode::dropView(const std::string& viewName,
                                                       bool isMaterialized) {
    auto r =
        ddl(*this, [&](dearsql::IDatabase& db) { return db.dropView(viewName, isMaterialized); });
    if (r.first) {
        if (isMaterialized)
            startMaterializedViewsLoadAsync(true);
        else
            startViewsLoadAsync(true);
    }
    return r;
}

bool LibDatabaseNode::hasPendingAsyncWork() const {
    return tablesLoader.isRunning() || viewsLoader.isRunning() ||
           materializedViewsLoader.isRunning() || sequencesLoader.isRunning() ||
           routinesLoader.isRunning();
}

void LibDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
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
    if (!it->second.isRunning())
        tableRefreshLoaders.erase(it);
}

void LibDatabaseNode::waitForLoaders() {
    cancelLoaders();
    tablesLoader.wait();
    viewsLoader.wait();
    materializedViewsLoader.wait();
    sequencesLoader.wait();
    routinesLoader.wait();
    for (auto& loader : tableRefreshLoaders | std::views::values)
        loader.wait();
}
