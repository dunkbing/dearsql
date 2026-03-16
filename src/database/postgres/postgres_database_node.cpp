#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <libpq-fe.h>
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

        Logger::info(std::format("Async schema loading completed for database {}. Found {} schemas",
                                 name, schemas.size()));
        schemasLoaded = true;
        if (refreshChildrenAfterSchemasLoad) {
            refreshChildrenAfterSchemasLoad = false;
            triggerChildSchemaRefresh();
        }
    });
}

void PostgresDatabaseNode::startSchemasLoadAsync(bool forceRefresh, bool refreshChildren) {
    Logger::debug("startSchemasLoadAsync for database: " + name +
                  (forceRefresh ? " (force refresh)" : "") +
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

            Logger::debug("Found " + std::to_string(schemaNames.size()) + " schemas in database " +
                          name);

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

    Logger::debug(std::format("initializeConnectionPool {}", info.buildConnectionString()));
    if (connectionPool) {
        return;
    }

    constexpr size_t poolSize = 3;
    std::string connStr = buildPqConnStr(info);

    connectionPool = std::make_unique<ConnectionPool<PGconn*>>(
        poolSize,
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
    Logger::debug(std::format("Triggering child schema refresh for database: {}", name));

    // Invalidate caches when schemas are refreshed
    allTablesCached = false;
    allViewsCached = false;
    allSequencesCached = false;

    // loop through all schemas and trigger refresh for tables, views, and sequences
    for (auto& schema : schemas) {
        if (schema) {
            Logger::debug(std::format("Refreshing schema: {}", schema->name));
            schema->startTablesLoadAsync(true);
            schema->startViewsLoadAsync(true);
            schema->startMaterializedViewsLoadAsync(true);
            schema->startSequencesLoadAsync(true);
        }
    }

    Logger::info(
        std::format("Triggered refresh for {} schemas in database {}", schemas.size(), name));
}

// ========== IDatabaseNode Implementation ==========

std::string PostgresDatabaseNode::getFullPath() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().name + "." + name;
    }
    return name;
}

DatabaseType PostgresDatabaseNode::getDatabaseType() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().type;
    }
    return DatabaseType::POSTGRESQL;
}

void PostgresDatabaseNode::rebuildTablesCache() const {
    allTables.clear();
    for (const auto& schema : schemas) {
        if (schema && schema->tablesLoaded) {
            for (const auto& table : schema->tables) {
                Table t = table;
                t.name = schema->name + "." + table.name;
                allTables.push_back(std::move(t));
            }
        }
    }
    allTablesCached = true;
}

void PostgresDatabaseNode::rebuildViewsCache() const {
    allViews.clear();
    for (const auto& schema : schemas) {
        if (schema && schema->viewsLoaded) {
            for (const auto& view : schema->views) {
                Table v = view;
                v.name = schema->name + "." + view.name;
                allViews.push_back(std::move(v));
            }
            // Also include materialized views
            if (schema->materializedViewsLoaded) {
                for (const auto& mv : schema->materializedViews) {
                    Table v = mv;
                    v.name = schema->name + "." + mv.name;
                    allViews.push_back(std::move(v));
                }
            }
        }
    }
    allViewsCached = true;
}

void PostgresDatabaseNode::rebuildSequencesCache() const {
    allSequences.clear();
    for (const auto& schema : schemas) {
        if (schema && schema->sequencesLoaded) {
            for (const auto& seq : schema->sequences) {
                allSequences.push_back(schema->name + "." + seq);
            }
        }
    }
    allSequencesCached = true;
}

std::vector<Table>& PostgresDatabaseNode::getTables() {
    if (!allTablesCached) {
        rebuildTablesCache();
    }
    return allTables;
}

const std::vector<Table>& PostgresDatabaseNode::getTables() const {
    if (!allTablesCached) {
        rebuildTablesCache();
    }
    return allTables;
}

std::vector<Table>& PostgresDatabaseNode::getViews() {
    if (!allViewsCached) {
        rebuildViewsCache();
    }
    return allViews;
}

const std::vector<Table>& PostgresDatabaseNode::getViews() const {
    if (!allViewsCached) {
        rebuildViewsCache();
    }
    return allViews;
}

const std::vector<std::string>& PostgresDatabaseNode::getSequences() const {
    if (!allSequencesCached) {
        rebuildSequencesCache();
    }
    return allSequences;
}

PostgresSchemaNode* PostgresDatabaseNode::findSchema(const std::string& schemaName) const {
    for (const auto& schema : schemas) {
        if (schema && schema->name == schemaName) {
            return schema.get();
        }
    }
    return nullptr;
}

std::pair<std::string, std::string>
PostgresDatabaseNode::parseQualifiedName(const std::string& qualifiedName) const {
    auto dot = qualifiedName.find('.');
    if (dot != std::string::npos) {
        return {qualifiedName.substr(0, dot), qualifiedName.substr(dot + 1)};
    }
    return {"public", qualifiedName};
}

std::vector<std::vector<std::string>>
PostgresDatabaseNode::getTableData(const std::string& tableName, int limit, int offset,
                                   const std::string& whereClause, const std::string& orderBy) {
    auto [schemaName, tblName] = parseQualifiedName(tableName);
    return getTableData(schemaName, tblName, limit, offset, whereClause, orderBy);
}

std::vector<std::string> PostgresDatabaseNode::getColumnNames(const std::string& tableName) {
    auto [schemaName, tblName] = parseQualifiedName(tableName);
    return getColumnNames(schemaName, tblName);
}

int PostgresDatabaseNode::getRowCount(const std::string& tableName,
                                      const std::string& whereClause) {
    auto [schemaName, tblName] = parseQualifiedName(tableName);
    return getRowCount(schemaName, tblName, whereClause);
}

std::pair<bool, std::string> PostgresDatabaseNode::createTable(const Table& table) {
    // Delegate to "public" schema by default
    auto* publicSchema = findSchema("public");
    if (publicSchema) {
        return publicSchema->createTable(table);
    }
    // Fallback: first available schema
    if (!schemas.empty() && schemas.front()) {
        return schemas.front()->createTable(table);
    }
    return {false, "No schemas available"};
}

bool PostgresDatabaseNode::isTablesLoaded() const {
    if (!schemasLoaded || schemas.empty()) {
        return false;
    }
    for (const auto& schema : schemas) {
        if (schema && !schema->tablesLoaded) {
            return false;
        }
    }
    return true;
}

bool PostgresDatabaseNode::isViewsLoaded() const {
    if (!schemasLoaded || schemas.empty()) {
        return false;
    }
    for (const auto& schema : schemas) {
        if (schema && !schema->viewsLoaded) {
            return false;
        }
    }
    return true;
}

bool PostgresDatabaseNode::isLoadingTables() const {
    for (const auto& schema : schemas) {
        if (schema && schema->tablesLoader.isRunning()) {
            return true;
        }
    }
    return false;
}

bool PostgresDatabaseNode::isLoadingViews() const {
    for (const auto& schema : schemas) {
        if (schema && schema->viewsLoader.isRunning()) {
            return true;
        }
    }
    return false;
}

void PostgresDatabaseNode::startTablesLoadAsync(bool force) {
    if (force) {
        allTablesCached = false;
    }
    for (auto& schema : schemas) {
        if (schema) {
            schema->startTablesLoadAsync(force);
        }
    }
}

void PostgresDatabaseNode::startViewsLoadAsync(bool force) {
    if (force) {
        allViewsCached = false;
    }
    for (auto& schema : schemas) {
        if (schema) {
            schema->startViewsLoadAsync(force);
        }
    }
}

void PostgresDatabaseNode::checkLoadingStatus() {
    bool needRebuildTables = false;
    bool needRebuildViews = false;
    for (auto& schema : schemas) {
        if (schema) {
            bool wasTblLoaded = schema->tablesLoaded;
            bool wasViewLoaded = schema->viewsLoaded;
            schema->checkLoadingStatus();
            if (!wasTblLoaded && schema->tablesLoaded) {
                needRebuildTables = true;
            }
            if (!wasViewLoaded && schema->viewsLoaded) {
                needRebuildViews = true;
            }
        }
    }
    if (needRebuildTables) {
        allTablesCached = false;
    }
    if (needRebuildViews) {
        allViewsCached = false;
    }
}

void PostgresDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    auto [schemaName, tblName] = parseQualifiedName(tableName);
    auto* schema = findSchema(schemaName);
    if (schema) {
        schema->startTableRefreshAsync(tblName);
    }
}

bool PostgresDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto [schemaName, tblName] = parseQualifiedName(tableName);
    auto* schema = findSchema(schemaName);
    return schema ? schema->isTableRefreshing(tblName) : false;
}

void PostgresDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto [schemaName, tblName] = parseQualifiedName(tableName);
    auto* schema = findSchema(schemaName);
    if (schema) {
        schema->checkTableRefreshStatusAsync(tblName);
        allTablesCached = false;
    }
}
