#include "database/mssql/mssql_database_node.hpp"
#include "database/db.hpp"
#include "database/mssql.hpp"
#include "database/sql_builder.hpp"
#include "mssql_utils.hpp"
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>
#include <unordered_map>

void MSSQLDatabaseNode::ensureConnectionPool() {
    if (!connectionPool && parentDb) {
        auto nodeInfo = parentDb->getConnectionInfo();
        nodeInfo.database = name;
        initializeConnectionPool(nodeInfo);
    }
}

void MSSQLDatabaseNode::initializeConnectionPool(const DatabaseConnectionInfo& info) {
    if (!parentDb)
        return;

    spdlog::debug("initializeConnectionPool (MSSQL) {}:{} db={}", info.host, info.port,
                  info.database);
    if (connectionPool)
        return;

    connectionPool = std::make_unique<ConnectionPool<DBPROCESS*>>(
        [info]() -> DBPROCESS* {
            MSSQLDatabase::initDbLib();
            return openDbLibConnection(info, info.database);
        },
        [](DBPROCESS* dbproc) { dbclose(dbproc); },
        [](DBPROCESS* dbproc) -> bool { return !dbdead(dbproc); });
}

ConnectionPool<DBPROCESS*>::Session MSSQLDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error(
            "MSSQLDatabaseNode::getSession: Connection pool not available for database: " + name);
    }
    return connectionPool->acquire();
}

// schema loading

void MSSQLDatabaseNode::checkSchemasStatusAsync() {
    schemasLoader.check([this](std::vector<std::unique_ptr<MSSQLSchemaNode>> result) {
        // merge: reuse existing schema nodes by name so raw pointers held by tabs stay valid
        std::unordered_map<std::string, std::unique_ptr<MSSQLSchemaNode>> existingByName;
        for (auto& s : schemas) {
            existingByName[s->name] = std::move(s);
        }
        schemas.clear();

        for (auto& newSchema : result) {
            auto it = existingByName.find(newSchema->name);
            if (it != existingByName.end()) {
                schemas.push_back(std::move(it->second));
                existingByName.erase(it);
            } else {
                schemas.push_back(std::move(newSchema));
            }
        }

        spdlog::debug("Async schema loading completed for database {}. Found {} schemas", name,
                      schemas.size());
        schemasLoaded = true;
        invalidateAggregatedObjects();
        if (refreshChildrenAfterSchemasLoad) {
            refreshChildrenAfterSchemasLoad = false;
            triggerChildSchemaRefresh();
        }
    });
}

void MSSQLDatabaseNode::startSchemasLoadAsync(bool forceRefresh, bool refreshChildren) {
    spdlog::debug("startSchemasLoadAsync for database: {}{}{}", name,
                  (forceRefresh ? " (force refresh)" : ""),
                  (refreshChildren ? " (refresh children)" : ""));
    if (!parentDb)
        return;

    if (schemasLoader.isRunning())
        return;

    if (forceRefresh) {
        schemasLoaded = false;
        lastSchemasError.clear();
    }
    refreshChildrenAfterSchemasLoad = refreshChildren;

    if (!forceRefresh && schemasLoaded)
        return;

    invalidateAggregatedObjects();

    schemasLoader.start([this]() {
        std::vector<std::unique_ptr<MSSQLSchemaNode>> result;

        if (!schemasLoader.isRunning())
            return result;

        try {
            ensureConnectionPool();

            if (!schemasLoader.isRunning())
                return result;

            std::vector<std::string> schemaNames;
            const std::string sqlQuery = "SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA "
                                         "WHERE CATALOG_NAME = DB_NAME() "
                                         "ORDER BY SCHEMA_NAME";

            {
                auto session = getSession();
                DBPROCESS* dbproc = session.get();
                if (execQuery(dbproc, sqlQuery) && dbresults(dbproc) == SUCCEED) {
                    while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                        if (!schemasLoader.isRunning())
                            return result;
                        schemaNames.push_back(colToString(dbproc, 1));
                    }
                }
                drainResults(dbproc);
            }

            spdlog::debug("Found {} schemas in database {}", schemaNames.size(), name);

            if (schemaNames.empty() || !schemasLoader.isRunning())
                return result;

            for (const auto& schemaName : schemaNames) {
                if (!schemasLoader.isRunning())
                    break;

                auto schema = std::make_unique<MSSQLSchemaNode>();
                schema->name = schemaName;
                schema->parentDbNode = this;
                result.push_back(std::move(schema));
            }
        } catch (const std::exception& e) {
            spdlog::error("Error getting schemas for database {}: {}", name, e.what());
            lastSchemasError = e.what();
        }
        return result;
    });
}

void MSSQLDatabaseNode::triggerChildSchemaRefresh() {
    for (auto& schema : schemas) {
        if (schema) {
            schema->startTablesLoadAsync(true);
            schema->startViewsLoadAsync(true);
        }
    }
}

// aggregated objects

void MSSQLDatabaseNode::invalidateAggregatedObjects() const {
    aggregatedObjectsDirty = true;
}

void MSSQLDatabaseNode::rebuildAggregatedObjects() const {
    if (!aggregatedObjectsDirty)
        return;

    allTables.clear();
    allViews.clear();

    for (const auto& schema : schemas) {
        if (!schema)
            continue;

        for (const auto& table : schema->tables) {
            Table qualifiedTable = table;
            qualifiedTable.name = schema->name + "." + table.name;
            allTables.push_back(std::move(qualifiedTable));
        }

        for (const auto& view : schema->views) {
            Table qualifiedView = view;
            qualifiedView.name = schema->name + "." + view.name;
            allViews.push_back(std::move(qualifiedView));
        }
    }

    aggregatedObjectsDirty = false;
}

std::vector<Table>& MSSQLDatabaseNode::getTables() {
    rebuildAggregatedObjects();
    return allTables;
}

const std::vector<Table>& MSSQLDatabaseNode::getTables() const {
    rebuildAggregatedObjects();
    return allTables;
}

std::vector<Table>& MSSQLDatabaseNode::getViews() {
    rebuildAggregatedObjects();
    return allViews;
}

const std::vector<Table>& MSSQLDatabaseNode::getViews() const {
    rebuildAggregatedObjects();
    return allViews;
}

// delegated IDatabaseNode methods

bool MSSQLDatabaseNode::isTablesLoaded() const {
    if (!schemasLoaded)
        return false;
    return std::ranges::all_of(schemas, [](const auto& s) { return !s || s->tablesLoaded; });
}

bool MSSQLDatabaseNode::isViewsLoaded() const {
    if (!schemasLoaded)
        return false;
    return std::ranges::all_of(schemas, [](const auto& s) { return !s || s->viewsLoaded; });
}

bool MSSQLDatabaseNode::isLoadingTables() const {
    if (schemasLoader.isRunning())
        return true;
    return std::ranges::any_of(schemas,
                               [](const auto& s) { return s && s->tablesLoader.isRunning(); });
}

bool MSSQLDatabaseNode::isLoadingViews() const {
    if (schemasLoader.isRunning())
        return true;
    return std::ranges::any_of(schemas,
                               [](const auto& s) { return s && s->viewsLoader.isRunning(); });
}

void MSSQLDatabaseNode::startTablesLoadAsync(bool force) {
    if (!schemasLoaded) {
        startSchemasLoadAsync(force, true);
        return;
    }
    for (auto& schema : schemas) {
        if (schema)
            schema->startTablesLoadAsync(force);
    }
}

void MSSQLDatabaseNode::startViewsLoadAsync(bool force) {
    if (!schemasLoaded) {
        startSchemasLoadAsync(force, true);
        return;
    }
    for (auto& schema : schemas) {
        if (schema)
            schema->startViewsLoadAsync(force);
    }
}

void MSSQLDatabaseNode::checkLoadingStatus() {
    checkSchemasStatusAsync();
    for (auto& schema : schemas) {
        if (schema)
            schema->checkLoadingStatus();
    }
}

const std::string& MSSQLDatabaseNode::getLastTablesError() const {
    if (!lastSchemasError.empty())
        return lastSchemasError;
    for (const auto& schema : schemas) {
        if (schema && !schema->lastTablesError.empty())
            return schema->lastTablesError;
    }
    static const std::string empty;
    return empty;
}

const std::string& MSSQLDatabaseNode::getLastViewsError() const {
    if (!lastSchemasError.empty())
        return lastSchemasError;
    for (const auto& schema : schemas) {
        if (schema && !schema->lastViewsError.empty())
            return schema->lastViewsError;
    }
    static const std::string empty;
    return empty;
}

MSSQLSchemaNode* MSSQLDatabaseNode::findSchema(const std::string& schemaName) {
    for (auto& s : schemas) {
        if (s && s->name == schemaName)
            return s.get();
    }
    return nullptr;
}

const MSSQLSchemaNode* MSSQLDatabaseNode::findSchema(const std::string& schemaName) const {
    for (const auto& s : schemas) {
        if (s && s->name == schemaName)
            return s.get();
    }
    return nullptr;
}

void MSSQLDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    auto [schema, tblName] = splitSchemaTable(tableName);
    if (auto* s = findSchema(schema))
        s->startTableRefreshAsync(tblName);
}

bool MSSQLDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto [schema, tblName] = splitSchemaTable(tableName);
    if (const auto* s = findSchema(schema))
        return s->isTableRefreshing(tblName);
    return false;
}

void MSSQLDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto [schema, tblName] = splitSchemaTable(tableName);
    if (auto* s = findSchema(schema))
        s->checkTableRefreshStatusAsync(tblName);
}

std::vector<std::vector<std::string>>
MSSQLDatabaseNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                const std::string& whereClause, const std::string& orderByClause) {
    auto [schema, tblName] = splitSchemaTable(tableName);
    if (auto* s = findSchema(schema))
        return s->getTableData(tblName, limit, offset, whereClause, orderByClause);
    return {};
}

std::vector<std::string> MSSQLDatabaseNode::getColumnNames(const std::string& tableName) {
    auto [schema, tblName] = splitSchemaTable(tableName);
    if (auto* s = findSchema(schema))
        return s->getColumnNames(tblName);
    return {};
}

int MSSQLDatabaseNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    auto [schema, tblName] = splitSchemaTable(tableName);
    if (auto* s = findSchema(schema))
        return s->getRowCount(tblName, whereClause);
    return 0;
}

QueryResult MSSQLDatabaseNode::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        ensureConnectionPool();
        auto session = getSession();
        result = executeQueryOnProcess(session.get(), query, rowLimit);
    } catch (const std::exception& e) {
        StatementResult r;
        r.success = false;
        r.errorMessage = e.what();
        result.statements.push_back(r);
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

std::pair<bool, std::string> MSSQLDatabaseNode::createTable(const Table& table) {
    try {
        const auto builder = createSQLBuilder(getDatabaseType());
        std::string sql = builder->createTable(table);

        auto result = executeQuery(sql);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

DatabaseInterface* MSSQLDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string MSSQLDatabaseNode::getFullPath() const {
    return name;
}
