#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <libpq-fe.h>
#include <ranges>
#include <spdlog/spdlog.h>
#include <unordered_map>

namespace {

    struct PgResultDeleter {
        void operator()(PGresult* r) const {
            if (r)
                PQclear(r);
        }
    };
    using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

    std::string pgValue(PGresult* res, int row, int col) {
        if (PQgetisnull(res, row, col)) {
            return "NULL";
        }
        return PQgetvalue(res, row, col);
    }

    // Build a libpq connection string from DatabaseConnectionInfo
    std::string buildPqConnStr(const DatabaseConnectionInfo& info) {
        std::string connStr = "host=" + info.host + " port=" + std::to_string(info.port);
        if (!info.database.empty()) {
            connStr += " dbname=" + info.database;
        } else {
            connStr += " dbname=postgres";
        }
        if (!info.username.empty()) {
            connStr += " user=" + info.username;
        }
        if (!info.password.empty()) {
            connStr += " password=" + info.password;
        }
        return connStr;
    }

    StatementResult extractPgResult(PGresult* res, int rowLimit) {
        StatementResult result;
        ExecStatusType status = PQresultStatus(res);

        if (status == PGRES_TUPLES_OK) {
            int nFields = PQnfields(res);
            int nRows = PQntuples(res);

            for (int col = 0; col < nFields; col++) {
                result.columnNames.emplace_back(PQfname(res, col));
            }

            int limit = std::min(nRows, rowLimit);
            for (int row = 0; row < limit; row++) {
                std::vector<std::string> rowData;
                rowData.reserve(nFields);
                for (int col = 0; col < nFields; col++) {
                    rowData.push_back(pgValue(res, row, col));
                }
                result.tableData.push_back(std::move(rowData));
            }

            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            if (nRows >= rowLimit) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
        } else if (status == PGRES_COMMAND_OK) {
            const char* affected = PQcmdTuples(res);
            if (affected && *affected) {
                result.message = std::format("{} row(s) affected", affected);
            } else {
                result.message = "Query executed successfully";
            }
        } else {
            result.success = false;
            result.errorMessage = PQresultErrorMessage(res);
        }
        return result;
    }

    std::pair<std::string, std::string> splitQualifiedObjectName(const std::string& objectName) {
        const auto dotPos = objectName.find('.');
        if (dotPos == std::string::npos)
            return {"", objectName};
        return {objectName.substr(0, dotPos), objectName.substr(dotPos + 1)};
    }

} // namespace

void PostgresDatabaseNode::checkSchemasStatusAsync() {
    schemasLoader.check([this](std::vector<std::unique_ptr<PostgresSchemaNode>> result) {
        // Merge: reuse existing schema nodes by name so raw pointers held by tabs stay valid
        std::unordered_map<std::string, std::unique_ptr<PostgresSchemaNode>> existingByName;
        for (auto& s : schemas) {
            existingByName[s->name] = std::move(s);
        }
        schemas.clear();

        for (auto& newSchema : result) {
            auto it = existingByName.find(newSchema->name);
            if (it != existingByName.end()) {
                // Preserve existing node (keeps raw pointers valid)
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

void PostgresDatabaseNode::startSchemasLoadAsync(bool forceRefresh, bool refreshChildren) {
    spdlog::debug("startSchemasLoadAsync for database: {}{}{}", name,
                  (forceRefresh ? " (force refresh)" : ""),
                  (refreshChildren ? " (refresh children)" : ""));
    if (!parentDb) {
        return;
    }

    // Don't start if already loading
    if (schemasLoader.isRunning()) {
        return;
    }

    // If force refresh, reset loaded state but keep old schemas alive
    // so that any raw pointers held by tabs remain valid until new schemas arrive
    if (forceRefresh) {
        schemasLoaded = false;
        lastSchemasError.clear();
    }
    refreshChildrenAfterSchemasLoad = refreshChildren;

    // Don't start if already loaded (unless force refresh)
    if (!forceRefresh && schemasLoaded) {
        return;
    }

    invalidateAggregatedObjects();

    // Start async loading using AsyncOperation
    schemasLoader.start([this, refreshChildren]() {
        std::vector<std::unique_ptr<PostgresSchemaNode>> result;

        // Check if we're still supposed to be loading
        if (!schemasLoader.isRunning()) {
            return result;
        }

        try {
            // Ensure we have a connection pool for the specific database
            if (!connectionPool) {
                auto nodeInfo = parentDb->getConnectionInfo();
                nodeInfo.database = name;
                initializeConnectionPool(nodeInfo);
            }

            if (!schemasLoader.isRunning()) {
                return result;
            }

            // Get schema names using the connection pool
            std::vector<std::string> schemaNames;
            const std::string sqlQuery =
                "SELECT schema_name FROM information_schema.schemata "
                "WHERE schema_name NOT IN ('information_schema', 'pg_catalog', 'pg_toast') "
                "AND schema_name NOT LIKE 'pg_temp_%' "
                "AND schema_name NOT LIKE 'pg_toast_temp_%' "
                "ORDER BY schema_name";

            {
                auto session = getSession();
                PGconn* conn = session.get();
                PgResultPtr res(PQexec(conn, sqlQuery.c_str()));
                if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                    int nRows = PQntuples(res.get());
                    for (int i = 0; i < nRows; i++) {
                        if (!schemasLoader.isRunning()) {
                            return result;
                        }
                        schemaNames.emplace_back(PQgetvalue(res.get(), i, 0));
                    }
                }
            }

            spdlog::debug("Found {} schemas in database {}", schemaNames.size(), name);

            if (schemaNames.empty() || !schemasLoader.isRunning()) {
                return result;
            }

            for (const auto& schemaName : schemaNames) {
                if (!schemasLoader.isRunning()) {
                    break;
                }

                auto schema = std::make_unique<PostgresSchemaNode>();
                schema->name = schemaName;
                schema->parentDbNode = this;

                result.push_back(std::move(schema));
            }
        } catch (const std::exception& e) {
            std::cerr << "Error getting schemas for database " << name << ": " << e.what()
                      << std::endl;
        }
        return result;
    });
}

ConnectionPool<PGconn*>::Session PostgresDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error("Connection pool not available for database: " + name);
    }
    return connectionPool->acquire();
}

void PostgresDatabaseNode::initializeConnectionPool(const DatabaseConnectionInfo& info) {
    if (!parentDb) {
        return;
    }

    spdlog::debug("initializeConnectionPool {}", info.buildConnectionString());
    if (connectionPool) {
        return;
    }

    std::string connStr = buildPqConnStr(info);

    connectionPool = std::make_unique<ConnectionPool<PGconn*>>(
        // factory
        [connStr]() -> PGconn* {
            PGconn* conn = PQconnectdb(connStr.c_str());
            if (PQstatus(conn) != CONNECTION_OK) {
                std::string err = PQerrorMessage(conn);
                PQfinish(conn);
                throw std::runtime_error("PostgreSQL connection failed: " + err);
            }
            return conn;
        },
        // closer
        [](PGconn* conn) { PQfinish(conn); },
        // validator
        [](PGconn* conn) { return PQstatus(conn) == CONNECTION_OK; });
}

QueryResult PostgresDatabaseNode::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        auto session = getSession();
        PGconn* conn = session.get();

        if (!PQsendQuery(conn, query.c_str())) {
            StatementResult r;
            r.success = false;
            r.errorMessage = PQerrorMessage(conn);
            result.statements.push_back(r);
            return result;
        }

        while (PGresult* raw = PQgetResult(conn)) {
            PgResultPtr res(raw);
            auto r = extractPgResult(res.get(), rowLimit);
            if (r.success || !r.errorMessage.empty()) {
                result.statements.push_back(std::move(r));
            }
        }
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

DatabaseInterface* PostgresDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string PostgresDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType PostgresDatabaseNode::getDatabaseType() const {
    if (parentDb)
        return parentDb->getConnectionInfo().type;
    return DatabaseType::POSTGRESQL;
}

void PostgresDatabaseNode::invalidateAggregatedObjects() const {
    aggregatedObjectsDirty = true;
}

void PostgresDatabaseNode::rebuildAggregatedObjects() const {
    if (!aggregatedObjectsDirty)
        return;

    allTables.clear();
    allViews.clear();
    allSequences.clear();

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

        for (const auto& sequence : schema->sequences)
            allSequences.push_back(schema->name + "." + sequence);
    }

    aggregatedObjectsDirty = false;
}

std::vector<Table>& PostgresDatabaseNode::getTables() {
    rebuildAggregatedObjects();
    return allTables;
}

const std::vector<Table>& PostgresDatabaseNode::getTables() const {
    rebuildAggregatedObjects();
    return allTables;
}

std::vector<Table>& PostgresDatabaseNode::getViews() {
    rebuildAggregatedObjects();
    return allViews;
}

const std::vector<Table>& PostgresDatabaseNode::getViews() const {
    rebuildAggregatedObjects();
    return allViews;
}

const std::vector<std::string>& PostgresDatabaseNode::getSequences() const {
    rebuildAggregatedObjects();
    return allSequences;
}

bool PostgresDatabaseNode::isTablesLoaded() const {
    if (!schemasLoaded)
        return false;
    return std::ranges::all_of(
        schemas, [](const auto& schema) { return schema && schema->isTablesLoaded(); });
}

bool PostgresDatabaseNode::isViewsLoaded() const {
    if (!schemasLoaded)
        return false;
    return std::ranges::all_of(
        schemas, [](const auto& schema) { return schema && schema->isViewsLoaded(); });
}

bool PostgresDatabaseNode::isLoadingTables() const {
    if (schemasLoader.isRunning())
        return true;
    return std::ranges::any_of(
        schemas, [](const auto& schema) { return schema && schema->isLoadingTables(); });
}

bool PostgresDatabaseNode::isLoadingViews() const {
    if (schemasLoader.isRunning())
        return true;
    return std::ranges::any_of(
        schemas, [](const auto& schema) { return schema && schema->isLoadingViews(); });
}

void PostgresDatabaseNode::startTablesLoadAsync(bool force) {
    invalidateAggregatedObjects();
    if (!schemasLoaded || force) {
        startSchemasLoadAsync(force, true);
        return;
    }

    for (auto& schema : schemas) {
        if (schema)
            schema->startTablesLoadAsync(force);
    }
}

void PostgresDatabaseNode::startViewsLoadAsync(bool force) {
    invalidateAggregatedObjects();
    if (!schemasLoaded || force) {
        startSchemasLoadAsync(force, true);
        return;
    }

    for (auto& schema : schemas) {
        if (schema)
            schema->startViewsLoadAsync(force);
    }
}

void PostgresDatabaseNode::checkLoadingStatus() {
    checkSchemasStatusAsync();
    for (auto& schema : schemas) {
        if (schema)
            schema->checkLoadingStatus();
    }
    invalidateAggregatedObjects();
}

const std::string& PostgresDatabaseNode::getLastTablesError() const {
    if (!lastSchemasError.empty())
        return lastSchemasError;

    aggregatedTablesError.clear();
    for (const auto& schema : schemas) {
        if (schema && !schema->getLastTablesError().empty()) {
            aggregatedTablesError = schema->getLastTablesError();
            break;
        }
    }
    return aggregatedTablesError;
}

const std::string& PostgresDatabaseNode::getLastViewsError() const {
    if (!lastSchemasError.empty())
        return lastSchemasError;

    aggregatedViewsError.clear();
    for (const auto& schema : schemas) {
        if (schema && !schema->getLastViewsError().empty()) {
            aggregatedViewsError = schema->getLastViewsError();
            break;
        }
    }
    return aggregatedViewsError;
}

std::vector<std::vector<std::string>>
PostgresDatabaseNode::getTableData(const std::string& tableName, int limit, int offset,
                                   const std::string& whereClause,
                                   const std::string& orderByClause) {
    auto [schemaName, objectName] = splitQualifiedObjectName(tableName);
    if (schemaName.empty()) {
        auto it = std::ranges::find_if(
            schemas, [](const auto& schema) { return schema && schema->name == "public"; });
        if (it != schemas.end() && *it)
            schemaName = (*it)->name;
        else if (!schemas.empty() && schemas.front())
            schemaName = schemas.front()->name;
    }

    if (schemaName.empty())
        return {};

    return getTableData(schemaName, objectName, limit, offset, whereClause, orderByClause);
}

std::vector<std::string> PostgresDatabaseNode::getColumnNames(const std::string& tableName) {
    auto [schemaName, objectName] = splitQualifiedObjectName(tableName);
    if (schemaName.empty()) {
        auto it = std::ranges::find_if(
            schemas, [](const auto& schema) { return schema && schema->name == "public"; });
        if (it != schemas.end() && *it)
            schemaName = (*it)->name;
        else if (!schemas.empty() && schemas.front())
            schemaName = schemas.front()->name;
    }

    if (schemaName.empty())
        return {};

    return getColumnNames(schemaName, objectName);
}

int PostgresDatabaseNode::getRowCount(const std::string& tableName,
                                      const std::string& whereClause) {
    auto [schemaName, objectName] = splitQualifiedObjectName(tableName);
    if (schemaName.empty()) {
        auto it = std::ranges::find_if(
            schemas, [](const auto& schema) { return schema && schema->name == "public"; });
        if (it != schemas.end() && *it)
            schemaName = (*it)->name;
        else if (!schemas.empty() && schemas.front())
            schemaName = schemas.front()->name;
    }

    if (schemaName.empty())
        return 0;

    return getRowCount(schemaName, objectName, whereClause);
}

void PostgresDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    auto [schemaName, objectName] = splitQualifiedObjectName(tableName);
    if (schemaName.empty())
        return;

    auto it = std::ranges::find_if(
        schemas, [&](const auto& schema) { return schema && schema->name == schemaName; });
    if (it != schemas.end() && *it) {
        (*it)->startTableRefreshAsync(objectName);
        invalidateAggregatedObjects();
    }
}

bool PostgresDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto [schemaName, objectName] = splitQualifiedObjectName(tableName);
    if (schemaName.empty())
        return false;

    auto it = std::ranges::find_if(
        schemas, [&](const auto& schema) { return schema && schema->name == schemaName; });
    return it != schemas.end() && *it && (*it)->isTableRefreshing(objectName);
}

void PostgresDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto [schemaName, objectName] = splitQualifiedObjectName(tableName);
    if (schemaName.empty())
        return;

    auto it = std::ranges::find_if(
        schemas, [&](const auto& schema) { return schema && schema->name == schemaName; });
    if (it != schemas.end() && *it) {
        (*it)->checkTableRefreshStatusAsync(objectName);
        invalidateAggregatedObjects();
    }
}

std::vector<std::vector<std::string>>
PostgresDatabaseNode::getTableData(const std::string& schemaName, const std::string& tableName,
                                   int limit, int offset, const std::string& whereClause,
                                   const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;

    try {
        std::string query = std::format(R"(SELECT * FROM "{}"."{}")", schemaName, tableName);
        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }
        if (!orderByClause.empty()) {
            query += " ORDER BY " + orderByClause;
        }
        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        auto session = getSession();
        PGconn* conn = session.get();
        PgResultPtr res(PQexec(conn, query.c_str()));
        if (!res || PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
            std::cerr << "Error getting table data: "
                      << (res ? PQresultErrorMessage(res.get()) : PQerrorMessage(conn))
                      << std::endl;
            return result;
        }

        int nFields = PQnfields(res.get());
        int nRows = PQntuples(res.get());
        for (int row = 0; row < nRows; row++) {
            std::vector<std::string> rowData;
            rowData.reserve(nFields);
            for (int col = 0; col < nFields; col++) {
                rowData.push_back(pgValue(res.get(), row, col));
            }
            result.push_back(std::move(rowData));
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }

    return result;
}

std::vector<std::string> PostgresDatabaseNode::getColumnNames(const std::string& schemaName,
                                                              const std::string& tableName) {
    std::vector<std::string> result;

    try {
        const std::string query =
            std::format("SELECT a.attname FROM pg_catalog.pg_attribute a "
                        "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
                        "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
                        "WHERE n.nspname = '{}' AND c.relname = '{}' "
                        "AND a.attnum > 0 AND NOT a.attisdropped "
                        "ORDER BY a.attnum",
                        schemaName, tableName);

        auto session = getSession();
        PGconn* conn = session.get();
        PgResultPtr res(PQexec(conn, query.c_str()));
        if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
            int nRows = PQntuples(res.get());
            for (int i = 0; i < nRows; i++) {
                result.emplace_back(PQgetvalue(res.get(), i, 0));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }

    return result;
}

int PostgresDatabaseNode::getRowCount(const std::string& schemaName, const std::string& tableName,
                                      const std::string& whereClause) {
    try {
        std::string query = std::format(R"(SELECT COUNT(*) FROM "{}"."{}")", schemaName, tableName);
        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }

        auto session = getSession();
        PGconn* conn = session.get();
        PgResultPtr res(PQexec(conn, query.c_str()));
        if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK && PQntuples(res.get()) > 0) {
            return std::atoi(PQgetvalue(res.get(), 0, 0));
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
    }
    return 0;
}

void PostgresDatabaseNode::triggerChildSchemaRefresh() {
    spdlog::debug("Triggering child schema refresh for database: {}", name);
    invalidateAggregatedObjects();

    // loop through all schemas and trigger refresh for tables, views, and sequences
    for (auto& schema : schemas) {
        if (schema) {
            spdlog::debug("Refreshing schema: {}", schema->name);
            schema->startTablesLoadAsync(true);
            schema->startViewsLoadAsync(true);
            schema->startMaterializedViewsLoadAsync(true);
            schema->startSequencesLoadAsync(true);
        }
    }

    spdlog::debug("Triggered refresh for {} schemas in database {}", schemas.size(), name);
}
