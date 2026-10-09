#pragma once

#include "lib_database_node.hpp"
#include <algorithm>

// a schema inside a SchemaDatabaseNode. it has no pool of its own: work runs on
// the database node's pooled handle through IDatabase::schema(), so a database
// with many schemas still holds only that node's few connections
class LibSchemaNode : public LibDatabaseNode {
protected:
    [[nodiscard]] virtual LibDatabaseNode* databaseNode() const = 0;

    void run(const std::function<void(dearsql::IDatabase&)>& fn) override {
        databaseNode()->withHandle([&](dearsql::IDatabase& db) {
            auto schema = db.schema(name);
            if (!schema)
                throw dearsql::Error("schema not found: " + name);
            fn(*schema);
        });
    }
    dearsql::DatabasePtr openHandle() final {
        return nullptr;
    }
};

// a database whose tables live in schemas (Postgres, MSSQL): pooled database
// handles plus one node per schema. as an IDatabaseNode it aggregates its schemas
// (the SQL editor completes across all of them)
template <typename SchemaT> class SchemaDatabaseNode : public LibDatabaseNode {
public:
    std::vector<std::unique_ptr<SchemaT>> schemas;
    bool schemasLoaded = false;
    AsyncOperation<Loaded<std::string>> schemasLoader;
    std::string lastSchemasError;

    // schemas run on our handles: join theirs before ours
    void waitForLoaders() {
        for (auto& s : schemas)
            s->waitForLoaders();
        schemasLoader.wait();
        LibDatabaseNode::waitForLoaders();
    }

    // refreshChildren reloads every schema's tables and views once the list lands
    void startSchemasLoadAsync(bool force = false, bool refreshChildren = false) {
        if (schemasLoader.isRunning() || (schemasLoaded && !force))
            return;
        refreshChildren_ = refreshChildren;
        schemasLoader.start([this] {
            return this->template load<std::string>("load schemas", [](dearsql::IDatabase& db) {
                std::vector<std::string> names;
                for (const auto& s : db.schemas())
                    names.push_back(s->name());
                return names;
            });
        });
    }

    void checkSchemasStatusAsync() {
        schemasLoader.check([this](Loaded<std::string> listed) {
            lastSchemasError = std::move(listed.error);
            std::vector<std::unique_ptr<SchemaT>> next;
            for (const auto& n : listed.items) {
                auto it =
                    std::ranges::find_if(schemas, [&](const auto& s) { return s && s->name == n; });
                next.push_back(it != schemas.end() ? std::move(*it) : makeSchema(n));
                if (refreshChildren_) {
                    next.back()->startTablesLoadAsync(true);
                    next.back()->startViewsLoadAsync(true);
                    if (next.back()->materializedViewsLoaded)
                        next.back()->startMaterializedViewsLoadAsync(true);
                    if (next.back()->sequencesLoaded)
                        next.back()->startSequencesLoadAsync(true);
                }
            }
            schemas = std::move(next);
            schemasLoaded = true;
            generation = nextGeneration();
        });
    }

    // ========== aggregated over schemas ==========

    std::vector<Table>& getTables() override {
        aggregate();
        return allTables_;
    }
    const std::vector<Table>& getTables() const override {
        aggregate();
        return allTables_;
    }
    std::vector<Table>& getViews() override {
        aggregate();
        return allViews_;
    }
    const std::vector<Table>& getViews() const override {
        aggregate();
        return allViews_;
    }
    const std::vector<std::string>& getSequences() const override {
        aggregate();
        return allSequences_;
    }

    [[nodiscard]] bool isTablesLoaded() const override {
        return schemasLoaded &&
               std::ranges::all_of(schemas, [](const auto& s) { return s->tablesLoaded; });
    }
    [[nodiscard]] bool isViewsLoaded() const override {
        return schemasLoaded &&
               std::ranges::all_of(schemas, [](const auto& s) { return s->viewsLoaded; });
    }
    [[nodiscard]] bool isLoadingTables() const override {
        return schemasLoader.isRunning() ||
               std::ranges::any_of(schemas, [](const auto& s) { return s->isLoadingTables(); });
    }
    [[nodiscard]] bool isLoadingViews() const override {
        return schemasLoader.isRunning() ||
               std::ranges::any_of(schemas, [](const auto& s) { return s->isLoadingViews(); });
    }
    [[nodiscard]] const std::string& getLastTablesError() const override {
        if (!lastSchemasError.empty())
            return lastSchemasError;
        for (const auto& s : schemas) {
            if (!s->lastTablesError.empty())
                return s->lastTablesError;
        }
        return lastSchemasError;
    }

    [[nodiscard]] const std::string& getLastViewsError() const override {
        for (const auto& s : schemas) {
            if (!s->lastViewsError.empty())
                return s->lastViewsError;
        }
        return lastViewsError;
    }

    void startTablesLoadAsync(bool force = false) override {
        if (!schemasLoaded) {
            startSchemasLoadAsync(force, true);
            return;
        }
        for (auto& s : schemas)
            s->startTablesLoadAsync(force);
    }
    void startViewsLoadAsync(bool force = false) override {
        if (!schemasLoaded) {
            startSchemasLoadAsync(force, true);
            return;
        }
        for (auto& s : schemas)
            s->startViewsLoadAsync(force);
    }
    void checkLoadingStatus() override {
        checkSchemasStatusAsync();
        LibDatabaseNode::checkLoadingStatus();
        for (auto& s : schemas)
            s->checkLoadingStatus();
    }

    // name may be "schema.table" or a bare table found in any schema
    void startTableRefreshAsync(const std::string& tableName) override {
        if (auto [schema, table] = locate(tableName); schema)
            schema->startTableRefreshAsync(table);
    }
    [[nodiscard]] bool isTableRefreshing(const std::string& tableName) const override {
        return std::ranges::any_of(schemas,
                                   [&](const auto& s) { return s->isTableRefreshing(tableName); });
    }

    [[nodiscard]] bool hasPendingAsyncWork() const {
        return LibDatabaseNode::hasPendingAsyncWork() || schemasLoader.isRunning() ||
               std::ranges::any_of(schemas, [](const auto& s) { return s->hasPendingAsyncWork(); });
    }
    void cancelLoaders() {
        LibDatabaseNode::cancelLoaders();
        schemasLoader.cancel();
        for (auto& s : schemas)
            s->cancelLoaders();
    }

protected:
    virtual std::unique_ptr<SchemaT> makeSchema(const std::string& name) = 0;

private:
    std::pair<SchemaT*, std::string> locate(const std::string& tableName) const {
        if (auto dot = tableName.find('.'); dot != std::string::npos) {
            for (const auto& s : schemas) {
                if (s->name == tableName.substr(0, dot))
                    return {s.get(), tableName.substr(dot + 1)};
            }
        }
        for (const auto& s : schemas) {
            if (std::ranges::any_of(s->tables, [&](const Table& t) { return t.name == tableName; }))
                return {s.get(), tableName};
        }
        return {nullptr, tableName};
    }

    // rebuilt only when a schema list or a schema's lists changed (newest stamp)
    void aggregate() const {
        uint64_t gen = generation;
        for (const auto& s : schemas)
            gen = std::max(gen, s->generation);
        if (gen == aggregatedGeneration_)
            return;
        aggregatedGeneration_ = gen;
        allTables_.clear();
        allViews_.clear();
        allSequences_.clear();
        for (const auto& s : schemas) {
            allTables_.insert(allTables_.end(), s->tables.begin(), s->tables.end());
            allViews_.insert(allViews_.end(), s->views.begin(), s->views.end());
            allSequences_.insert(allSequences_.end(), s->sequences.begin(), s->sequences.end());
        }
    }

    bool refreshChildren_ = false;
    mutable uint64_t aggregatedGeneration_ = ~0ull;
    mutable std::vector<Table> allTables_;
    mutable std::vector<Table> allViews_;
    mutable std::vector<std::string> allSequences_;
};
