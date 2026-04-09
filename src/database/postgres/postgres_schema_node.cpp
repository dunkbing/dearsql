#include "database/postgres/postgres_schema_node.hpp"
#include "database/db.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <format>
#include <iostream>
#include <libpq-fe.h>
#include <map>
#include <ranges>
#include <spdlog/spdlog.h>
#include <unordered_map>

namespace {

    // escape a PostgreSQL identifier: double any embedded quotes
    std::string quotePgId(const std::string& id) {
        std::string out = "\"";
        out.reserve(id.size() + 2);
        for (char c : id) {
            if (c == '"')
                out += '"';
            out += c;
        }
        out += '"';
        return out;
    }

    struct PgResultDeleter {
        void operator()(PGresult* r) const {
            if (r)
                PQclear(r);
        }
    };
    using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

    std::string pgValue(PGresult* res, int row, int col) {
        if (PQgetisnull(res, row, col)) {
            return std::string(NULL_SENTINEL);
        }
        return PQgetvalue(res, row, col);
    }

} // namespace

void PostgresSchemaNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        spdlog::debug("Async table loading completed for schema {}. Found {} tables", name,
                      tables.size());
        tablesLoaded = true;
    });
}

void PostgresSchemaNode::startTablesLoadAsync(const bool forceRefresh) {
    spdlog::debug("startTablesLoadAsync for schema: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDbNode) {
        return;
    }

    // Don't start if already loading
    if (tablesLoader.isRunning()) {
        return;
    }

    // If force refresh, clear existing tables and reset state
    if (forceRefresh) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }

    // Don't start if already loaded (unless force refresh)
    if (!forceRefresh && tablesLoaded) {
        return;
    }

    tables.clear();

    // Start async loading
    tablesLoader.start([this]() { return getTablesAsync(); });
}

std::vector<Table> PostgresSchemaNode::getTablesAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!tablesLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDbNode) {
            return result;
        }

        // Get table names using the connection pool
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = std::format(
            "SELECT tablename FROM pg_tables WHERE schemaname = '{}' ORDER BY tablename", name);

        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, tableNamesQuery.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!tablesLoader.isRunning()) {
                        return result;
                    }
                    tableNames.emplace_back(PQgetvalue(res.get(), i, 0));
                }
            }
        }

        spdlog::debug("Found {} tables in schema {}", tableNames.size(), name);

        if (tableNames.empty() || !tablesLoader.isRunning()) {
            return result;
        }

        // Build a single query to get all columns for all tables at once
        std::string sqlQuery =
            "SELECT "
            "c.table_name, "
            "c.column_name, "
            "c.data_type, "
            "c.is_nullable, "
            "CASE WHEN tc.constraint_type = 'PRIMARY KEY' THEN 'true' ELSE 'false' END "
            "as is_primary_key, "
            "CASE WHEN c.column_default LIKE 'nextval(%' OR c.is_identity = 'YES' "
            "THEN 'true' ELSE 'false' END as is_auto_increment "
            "FROM information_schema.columns c "
            "LEFT JOIN information_schema.key_column_usage kcu ON c.column_name = "
            "kcu.column_name AND c.table_name = kcu.table_name AND c.table_schema = "
            "kcu.table_schema "
            "LEFT JOIN information_schema.table_constraints tc ON kcu.constraint_name = "
            "tc.constraint_name AND tc.constraint_type = 'PRIMARY KEY' "
            "WHERE c.table_schema = '" +
            name + "' AND c.table_name IN (";

        // Add table names to the query
        for (size_t i = 0; i < tableNames.size(); ++i) {
            sqlQuery += "'" + tableNames[i] + "'";
            if (i < tableNames.size() - 1) {
                sqlQuery += ", ";
            }
        }
        sqlQuery += ") ORDER BY c.table_name, c.ordinal_position";

        // Execute the query using the connection pool
        std::unordered_map<std::string, std::vector<Column>> tableColumns;
        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, sqlQuery.c_str()));

            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!tablesLoader.isRunning()) {
                        break;
                    }

                    auto tableName = std::string(PQgetvalue(res.get(), i, 0));
                    Column col;
                    col.name = PQgetvalue(res.get(), i, 1);
                    col.type = PQgetvalue(res.get(), i, 2);
                    col.isNotNull = std::string(PQgetvalue(res.get(), i, 3)) == "NO";
                    col.isPrimaryKey = std::string(PQgetvalue(res.get(), i, 4)) == "true";
                    col.isAutoIncrement = std::string(PQgetvalue(res.get(), i, 5)) == "true";

                    tableColumns[tableName].push_back(col);
                }
            }
        }

        // Build a query to get all foreign keys for all tables at once
        std::string fkQuery =
            "SELECT "
            "tc.table_name, "
            "kcu.column_name AS source_column, "
            "ccu.table_name AS target_table, "
            "ccu.column_name AS target_column, "
            "tc.constraint_name "
            "FROM information_schema.table_constraints AS tc "
            "JOIN information_schema.key_column_usage AS kcu "
            "  ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "JOIN information_schema.constraint_column_usage AS ccu "
            "  ON ccu.constraint_name = tc.constraint_name AND ccu.table_schema = tc.table_schema "
            "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_schema = '" +
            name + "' AND tc.table_name IN (";

        for (size_t i = 0; i < tableNames.size(); ++i) {
            fkQuery += "'" + tableNames[i] + "'";
            if (i < tableNames.size() - 1) {
                fkQuery += ", ";
            }
        }
        fkQuery += ") ORDER BY tc.table_name";

        // Execute the foreign keys query
        std::unordered_map<std::string, std::vector<ForeignKey>> tableForeignKeys;
        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, fkQuery.c_str()));

            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!tablesLoader.isRunning()) {
                        break;
                    }

                    auto tableName = std::string(PQgetvalue(res.get(), i, 0));
                    ForeignKey fk;
                    fk.sourceColumn = PQgetvalue(res.get(), i, 1);
                    fk.targetTable = PQgetvalue(res.get(), i, 2);
                    fk.targetColumn = PQgetvalue(res.get(), i, 3);
                    fk.name = PQgetvalue(res.get(), i, 4);

                    tableForeignKeys[tableName].push_back(fk);
                }
            }
        }

        // Build the result tables
        for (const auto& tableName : tableNames) {
            if (!tablesLoader.isRunning()) {
                break;
            }

            Table table;
            table.name = tableName;
            table.fullName = parentDbNode->name + "." + name + "." + tableName;
            table.columns = std::move(tableColumns[tableName]);
            table.foreignKeys = std::move(tableForeignKeys[tableName]);

            result.push_back(table);
            spdlog::debug("Loaded table: {} with {} columns and {} foreign keys", tableName,
                          table.columns.size(), table.foreignKeys.size());
        }

    } catch (const std::exception& e) {
        std::cerr << "Error getting tables with columns for schema " << name << ": " << e.what()
                  << std::endl;
        lastTablesError = e.what();
    }

    return result;
}

void PostgresSchemaNode::checkViewsStatusAsync() {
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        spdlog::debug("Async view loading completed for schema {}. Found {} views", name,
                      views.size());
        viewsLoaded = true;
    });
}

void PostgresSchemaNode::startViewsLoadAsync(bool forceRefresh) {
    spdlog::debug("startViewsLoadAsync for schema: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDbNode) {
        return;
    }

    // Don't start if already loading
    if (viewsLoader.isRunning()) {
        return;
    }

    // If force refresh, clear existing views and reset state
    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    // Don't start if already loaded (unless force refresh)
    if (!forceRefresh && viewsLoaded) {
        return;
    }

    views.clear();

    // Start async loading
    viewsLoader.start([this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> PostgresSchemaNode::getViewsWithColumnsAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!viewsLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDbNode) {
            return result;
        }

        // Get view names using the connection pool
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = std::format(
            "SELECT viewname FROM pg_views WHERE schemaname = '{}' ORDER BY viewname", name);

        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, viewNamesQuery.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!viewsLoader.isRunning()) {
                        return result;
                    }
                    viewNames.emplace_back(PQgetvalue(res.get(), i, 0));
                }
            }
        }

        spdlog::debug("Found {} views in schema {}", viewNames.size(), name);

        if (viewNames.empty() || !viewsLoader.isRunning()) {
            return result;
        }

        // Build a single query to get all columns for all views at once
        std::string sqlQuery = "SELECT "
                               "c.table_name, "
                               "c.column_name, "
                               "c.data_type, "
                               "c.is_nullable "
                               "FROM information_schema.columns c "
                               "WHERE c.table_schema = '" +
                               name + "' AND c.table_name IN (";

        // Add view names to the query
        for (size_t i = 0; i < viewNames.size(); ++i) {
            sqlQuery += "'" + viewNames[i] + "'";
            if (i < viewNames.size() - 1) {
                sqlQuery += ", ";
            }
        }
        sqlQuery += ") ORDER BY c.table_name, c.ordinal_position";

        // Execute the query using the connection pool
        std::unordered_map<std::string, std::vector<Column>> viewColumns;
        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, sqlQuery.c_str()));

            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!viewsLoader.isRunning()) {
                        break;
                    }

                    auto viewName = std::string(PQgetvalue(res.get(), i, 0));
                    Column col;
                    col.name = PQgetvalue(res.get(), i, 1);
                    col.type = PQgetvalue(res.get(), i, 2);
                    col.isNotNull = std::string(PQgetvalue(res.get(), i, 3)) == "NO";
                    col.isPrimaryKey = false; // Views don't have primary keys

                    viewColumns[viewName].push_back(col);
                }
            }
        }

        // Build the result views
        for (const auto& viewName : viewNames) {
            if (!viewsLoader.isRunning()) {
                break;
            }

            Table view;
            view.name = viewName;
            view.fullName = parentDbNode->name + "." + name + "." + viewName;
            view.columns = std::move(viewColumns[viewName]);

            result.push_back(view);
            spdlog::debug("Loaded view: {} with {} columns", viewName, view.columns.size());
        }

    } catch (const std::exception& e) {
        std::cerr << "Error getting views with columns for schema " << name << ": " << e.what()
                  << std::endl;
        lastViewsError = e.what();
    }

    return result;
}

void PostgresSchemaNode::checkMaterializedViewsStatusAsync() {
    materializedViewsLoader.check([this](const std::vector<Table>& result) {
        materializedViews = result;
        spdlog::debug(
            "Async materialized view loading completed for schema {}. Found {} materialized views",
            name, materializedViews.size());
        materializedViewsLoaded = true;
    });
}

void PostgresSchemaNode::startMaterializedViewsLoadAsync(bool forceRefresh) {
    spdlog::debug("startMaterializedViewsLoadAsync for schema: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDbNode) {
        return;
    }

    if (materializedViewsLoader.isRunning()) {
        return;
    }

    if (forceRefresh) {
        materializedViews.clear();
        materializedViewsLoaded = false;
        lastMaterializedViewsError.clear();
    }

    if (!forceRefresh && materializedViewsLoaded) {
        return;
    }

    materializedViews.clear();
    materializedViewsLoader.start([this]() { return getMaterializedViewsWithColumnsAsync(); });
}

std::vector<Table> PostgresSchemaNode::getMaterializedViewsWithColumnsAsync() {
    std::vector<Table> result;

    if (!materializedViewsLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDbNode) {
            return result;
        }

        std::vector<std::string> matviewNames;
        const std::string matviewNamesQuery = std::format(
            "SELECT matviewname FROM pg_matviews WHERE schemaname = '{}' ORDER BY matviewname",
            name);

        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, matviewNamesQuery.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!materializedViewsLoader.isRunning()) {
                        return result;
                    }
                    matviewNames.emplace_back(PQgetvalue(res.get(), i, 0));
                }
            }
        }

        spdlog::debug("Found {} materialized views in schema {}", matviewNames.size(), name);

        if (matviewNames.empty() || !materializedViewsLoader.isRunning()) {
            return result;
        }

        // Get columns for materialized views using pg_attribute (information_schema doesn't include
        // them)
        std::string sqlQuery = "SELECT c.relname AS table_name, a.attname AS column_name, "
                               "pg_catalog.format_type(a.atttypid, a.atttypmod) AS data_type, "
                               "a.attnotnull AS is_not_null "
                               "FROM pg_catalog.pg_attribute a "
                               "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
                               "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
                               "WHERE n.nspname = '" +
                               name +
                               "' AND c.relkind = 'm' "
                               "AND a.attnum > 0 AND NOT a.attisdropped "
                               "AND c.relname IN (";

        for (size_t i = 0; i < matviewNames.size(); ++i) {
            sqlQuery += "'" + matviewNames[i] + "'";
            if (i < matviewNames.size() - 1) {
                sqlQuery += ", ";
            }
        }
        sqlQuery += ") ORDER BY c.relname, a.attnum";

        std::unordered_map<std::string, std::vector<Column>> matviewColumns;
        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, sqlQuery.c_str()));

            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!materializedViewsLoader.isRunning()) {
                        break;
                    }

                    auto mvName = std::string(PQgetvalue(res.get(), i, 0));
                    Column col;
                    col.name = PQgetvalue(res.get(), i, 1);
                    col.type = PQgetvalue(res.get(), i, 2);
                    col.isNotNull = std::string(PQgetvalue(res.get(), i, 3)) == "t";
                    col.isPrimaryKey = false;

                    matviewColumns[mvName].push_back(col);
                }
            }
        }

        for (const auto& mvName : matviewNames) {
            if (!materializedViewsLoader.isRunning()) {
                break;
            }

            Table mv;
            mv.name = mvName;
            mv.fullName = parentDbNode->name + "." + name + "." + mvName;
            mv.columns = std::move(matviewColumns[mvName]);

            result.push_back(mv);
            spdlog::debug("Loaded materialized view: {} with {} columns", mvName,
                          mv.columns.size());
        }

    } catch (const std::exception& e) {
        std::cerr << "Error getting materialized views for schema " << name << ": " << e.what()
                  << std::endl;
        lastMaterializedViewsError = e.what();
    }

    return result;
}

void PostgresSchemaNode::checkSequencesStatusAsync() {
    sequencesLoader.check([this](const std::vector<std::string>& result) {
        sequences = result;
        spdlog::debug("Async sequence loading completed for schema {}. Found {} sequences", name,
                      sequences.size());
        sequencesLoaded = true;
    });
}

void PostgresSchemaNode::startSequencesLoadAsync(bool forceRefresh) {
    spdlog::debug("startSequencesLoadAsync for schema: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDbNode) {
        return;
    }

    // Don't start if already loading
    if (sequencesLoader.isRunning()) {
        return;
    }

    // If force refresh, clear existing sequences and reset state
    if (forceRefresh) {
        sequences.clear();
        sequencesLoaded = false;
        lastSequencesError.clear();
    }

    // Don't start if already loaded (unless force refresh)
    if (!forceRefresh && sequencesLoaded) {
        return;
    }

    sequences.clear();

    // Start async loading
    sequencesLoader.start([this]() { return getSequencesAsync(); });
}

std::vector<std::string> PostgresSchemaNode::getSequencesAsync() {
    std::vector<std::string> result;

    // Check if we're still supposed to be loading
    if (!sequencesLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDbNode) {
            return result;
        }

        // Get sequence names using the connection pool
        const std::string sequencesQuery = std::format(
            "SELECT sequencename FROM pg_sequences WHERE schemaname = '{}' ORDER BY sequencename",
            name);

        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, sequencesQuery.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!sequencesLoader.isRunning()) {
                        return result;
                    }
                    result.emplace_back(PQgetvalue(res.get(), i, 0));
                }
            }
        }

        spdlog::debug("Found {} sequences in schema {}", result.size(), name);

    } catch (const std::exception& e) {
        std::cerr << "Error getting sequences for schema " << name << ": " << e.what() << std::endl;
        lastSequencesError = e.what();
    }

    return result;
}

void PostgresSchemaNode::checkRoutinesStatusAsync() {
    routinesLoader.check([this](const std::vector<Routine>& result) {
        routines = result;
        spdlog::debug("Async routine loading completed for schema {}. Found {} routines", name,
                      routines.size());
        routinesLoaded = true;
    });
}

void PostgresSchemaNode::startRoutinesLoadAsync(bool forceRefresh) {
    spdlog::debug("startRoutinesLoadAsync for schema: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDbNode || routinesLoader.isRunning()) {
        return;
    }

    if (forceRefresh) {
        routines.clear();
        routinesLoaded = false;
        lastRoutinesError.clear();
    }

    if (!forceRefresh && routinesLoaded) {
        return;
    }

    routines.clear();
    routinesLoader.start([this]() { return getRoutinesAsync(); });
}

std::vector<Routine> PostgresSchemaNode::getRoutinesAsync() {
    std::vector<Routine> result;

    if (!routinesLoader.isRunning()) {
        return result;
    }

    try {
        if (!parentDbNode) {
            return result;
        }

        const std::string query = std::format(
            "SELECT p.proname, "
            "p.proname || '(' || COALESCE(pg_catalog.pg_get_function_identity_arguments(p.oid), "
            "'') || ')' AS signature, "
            "CASE p.prokind WHEN 'f' THEN 'FUNCTION' ELSE 'PROCEDURE' END AS kind, "
            "pg_catalog.pg_get_function_result(p.oid) AS return_type "
            "FROM pg_catalog.pg_proc p "
            "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
            "WHERE n.nspname = '{}' AND p.prokind IN ('f', 'p') "
            "ORDER BY p.prokind, p.proname",
            name);

        {
            auto session = parentDbNode->getSession();
            PGconn* conn = session.get();
            PgResultPtr res(PQexec(conn, query.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    if (!routinesLoader.isRunning()) {
                        return result;
                    }
                    Routine r;
                    r.name = PQgetvalue(res.get(), i, 0);
                    r.signature = PQgetvalue(res.get(), i, 1);
                    std::string kindStr = PQgetvalue(res.get(), i, 2);
                    r.kind =
                        (kindStr == "PROCEDURE") ? RoutineKind::Procedure : RoutineKind::Function;
                    r.returnType = PQgetvalue(res.get(), i, 3);
                    result.push_back(std::move(r));
                }
            }
        }

        spdlog::debug("Found {} routines in schema {}", result.size(), name);

    } catch (const std::exception& e) {
        spdlog::error("Error getting routines for schema {}: {}", name, e.what());
        lastRoutinesError = e.what();
    }

    return result;
}

void PostgresSchemaNode::startTableRefreshAsync(const std::string& tableName) {
    spdlog::debug("Starting async refresh for table: {}.{}", name, tableName);

    // Check if already refreshing
    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning()) {
        return;
    }

    // Start async loading
    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

void PostgresSchemaNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end()) {
        return;
    }

    it->second.check([this, tableName](const Table& refreshedTable) {
        // Find the table in the tables vector and update it
        const auto tableIt = std::ranges::find_if(
            tables, [&tableName](const Table& t) { return t.name == tableName; });

        if (tableIt != tables.end()) {
            *tableIt = refreshedTable;
            spdlog::debug("Table {}.{} refreshed successfully", name, tableName);
        }

        // Clean up the loader
        tableRefreshLoaders.erase(tableName);
    });
}

Table PostgresSchemaNode::refreshTableAsync(const std::string& tableName) {
    spdlog::debug("Refreshing table: {}.{}", name, tableName);

    Table refreshedTable;
    refreshedTable.name = tableName;

    if (!parentDbNode) {
        spdlog::error("Cannot refresh table: no parent database node");
        return refreshedTable;
    }

    try {
        auto session = parentDbNode->getSession();
        PGconn* conn = session.get();

        // Get table columns
        const std::string columnsQuery =
            std::format("SELECT column_name, data_type, is_nullable, column_default, "
                        "CASE WHEN column_default LIKE 'nextval(%%' OR is_identity = 'YES' "
                        "THEN 'true' ELSE 'false' END as is_auto_increment "
                        "FROM information_schema.columns "
                        "WHERE table_schema = '{}' AND table_name = '{}' "
                        "ORDER BY ordinal_position",
                        name, tableName);

        {
            PgResultPtr res(PQexec(conn, columnsQuery.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    Column col;
                    col.name = PQgetvalue(res.get(), i, 0);
                    col.type = PQgetvalue(res.get(), i, 1);
                    col.isNotNull = std::string(PQgetvalue(res.get(), i, 2)) == "NO";
                    col.isAutoIncrement = std::string(PQgetvalue(res.get(), i, 4)) == "true";
                    refreshedTable.columns.push_back(col);
                }
            }
        }

        // Get primary key information
        const std::string pkQuery = std::format(
            "SELECT a.attname "
            "FROM pg_index i "
            "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
            "WHERE i.indrelid = '\"{}\".\"{}\"'::regclass AND i.indisprimary",
            name, tableName);

        {
            PgResultPtr res(PQexec(conn, pkQuery.c_str()));
            std::vector<std::string> pkColumns;
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    pkColumns.emplace_back(PQgetvalue(res.get(), i, 0));
                }
            }

            // Mark columns as primary key
            for (auto& col : refreshedTable.columns) {
                if (std::ranges::find(pkColumns, col.name) != pkColumns.end()) {
                    col.isPrimaryKey = true;
                }
            }
        }

        // Get indexes
        const std::string indexQuery =
            std::format("SELECT i.relname, a.attname, ix.indisunique "
                        "FROM pg_class t "
                        "JOIN pg_index ix ON t.oid = ix.indrelid "
                        "JOIN pg_class i ON i.oid = ix.indexrelid "
                        "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = ANY(ix.indkey) "
                        "WHERE t.relkind = 'r' AND t.relnamespace = '{}'::regnamespace "
                        "AND t.relname = '{}' AND NOT ix.indisprimary "
                        "ORDER BY i.relname, a.attnum",
                        name, tableName);

        {
            PgResultPtr res(PQexec(conn, indexQuery.c_str()));
            std::unordered_map<std::string, Index> indexMap;

            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    auto indexName = std::string(PQgetvalue(res.get(), i, 0));
                    auto columnName = std::string(PQgetvalue(res.get(), i, 1));
                    bool isUnique = std::string(PQgetvalue(res.get(), i, 2)) == "t";

                    if (!indexMap.contains(indexName)) {
                        Index idx;
                        idx.name = indexName;
                        idx.isUnique = isUnique;
                        indexMap[indexName] = idx;
                    }
                    indexMap[indexName].columns.push_back(columnName);
                }
            }

            for (auto& idx : indexMap | std::views::values) {
                refreshedTable.indexes.push_back(idx);
            }
        }

        // Get foreign keys
        const std::string fkQuery = std::format(
            "SELECT kcu.column_name, ccu.table_name, ccu.column_name, tc.constraint_name "
            "FROM information_schema.table_constraints AS tc "
            "JOIN information_schema.key_column_usage AS kcu "
            "  ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "JOIN information_schema.constraint_column_usage AS ccu "
            "  ON ccu.constraint_name = tc.constraint_name AND ccu.table_schema = tc.table_schema "
            "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_schema = '{}' AND tc.table_name "
            "= '{}'",
            name, tableName);

        {
            PgResultPtr res(PQexec(conn, fkQuery.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int nRows = PQntuples(res.get());
                for (int i = 0; i < nRows; i++) {
                    ForeignKey fk;
                    fk.sourceColumn = PQgetvalue(res.get(), i, 0);
                    fk.targetTable = PQgetvalue(res.get(), i, 1);
                    fk.targetColumn = PQgetvalue(res.get(), i, 2);
                    fk.name = PQgetvalue(res.get(), i, 3);
                    refreshedTable.foreignKeys.push_back(fk);
                }
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("Error refreshing table {}.{}: {}", name, tableName, e.what());
        throw;
    }

    return refreshedTable;
}

bool PostgresSchemaNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

std::vector<std::vector<std::string>>
PostgresSchemaNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                 const std::string& whereClause, const std::string& orderByClause) {
    if (!parentDbNode) {
        return {};
    }
    return parentDbNode->getTableData(name, tableName, limit, offset, whereClause, orderByClause);
}

std::vector<std::string> PostgresSchemaNode::getColumnNames(const std::string& tableName) {
    if (!parentDbNode) {
        return {};
    }
    return parentDbNode->getColumnNames(name, tableName);
}

int PostgresSchemaNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    if (!parentDbNode) {
        return 0;
    }
    return parentDbNode->getRowCount(name, tableName, whereClause);
}

QueryResult PostgresSchemaNode::executeQuery(const std::string& query, int rowLimit) {
    if (!parentDbNode) {
        QueryResult result;
        StatementResult r;
        r.success = false;
        r.errorMessage = "No database connection";
        result.statements.push_back(std::move(r));
        return result;
    }
    // Escape double quotes in schema name for safe identifier quoting
    std::string escapedName = name;
    for (std::string::size_type pos = 0; (pos = escapedName.find('"', pos)) != std::string::npos;
         pos += 2) {
        escapedName.insert(pos, 1, '"');
    }
    std::string wrappedQuery = std::format("SET search_path TO \"{}\"; {}", escapedName, query);
    auto result = parentDbNode->executeQuery(wrappedQuery, rowLimit);
    // Drop the SET search_path result (first entry) — it's an internal detail
    if (result.statements.size() > 1) {
        result.statements.erase(result.statements.begin());
    }
    return result;
}

std::pair<bool, std::string> PostgresSchemaNode::createTable(const Table& table) {
    if (!parentDbNode) {
        return {false, "No database connection"};
    }

    try {
        const auto builder = createSQLBuilder(DatabaseType::POSTGRESQL);
        std::string sql = builder->createTable(table, name);

        auto result = parentDbNode->executeQuery(sql);
        bool success = result.success();
        std::string error = result.errorMessage();
        if (!success) {
            return {false, error};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

DatabaseInterface* PostgresSchemaNode::ownerDatabase() const {
    return (parentDbNode && parentDbNode->parentDb) ? parentDbNode->parentDb : nullptr;
}

std::string PostgresSchemaNode::getFullPath() const {
    if (!parentDbNode) {
        return name;
    }
    return parentDbNode->name + "." + name;
}

DatabaseType PostgresSchemaNode::getDatabaseType() const {
    if (parentDbNode && parentDbNode->parentDb) {
        return parentDbNode->parentDb->getConnectionInfo().type;
    }
    return DatabaseType::POSTGRESQL;
}

void PostgresSchemaNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
    checkMaterializedViewsStatusAsync();
    checkSequencesStatusAsync();
    checkRoutinesStatusAsync();
}

std::pair<bool, std::string> PostgresSchemaNode::renameSchema(const std::string& newName) {
    auto sql = std::format(R"(ALTER SCHEMA "{}" RENAME TO "{}")", name, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        if (parentDbNode) {
            parentDbNode->startSchemasLoadAsync(true, false);
        }
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> PostgresSchemaNode::dropSchema() {
    auto sql = std::format(R"(DROP SCHEMA "{}" CASCADE)", name);
    auto r = executeQuery(sql);
    if (r.success()) {
        if (parentDbNode) {
            parentDbNode->startSchemasLoadAsync(true, false);
        }
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> PostgresSchemaNode::renameTable(const std::string& oldName,
                                                             const std::string& newName) {
    auto sql = std::format(R"(ALTER TABLE "{}"."{}" RENAME TO "{}")", name, oldName, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> PostgresSchemaNode::dropTable(const std::string& tableName) {
    auto sql = std::format(R"(DROP TABLE "{}"."{}")", name, tableName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> PostgresSchemaNode::truncateTable(const std::string& tableName) {
    auto sql = std::format("TRUNCATE TABLE ONLY {}.{}", quotePgId(name), quotePgId(tableName));
    auto r = executeQuery(sql);
    if (r.success())
        return {true, ""};
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> PostgresSchemaNode::dropColumn(const std::string& tableName,
                                                            const std::string& columnName) {
    auto sql =
        std::format(R"(ALTER TABLE "{}"."{}" DROP COLUMN "{}")", name, tableName, columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> PostgresSchemaNode::dropView(const std::string& viewName,
                                                          bool isMaterialized) {
    const auto keyword = isMaterialized ? "MATERIALIZED VIEW" : "VIEW";
    auto sql = std::format(R"(DROP {} "{}"."{}")", keyword, name, viewName);
    auto r = executeQuery(sql);
    if (r.success()) {
        if (isMaterialized) {
            startMaterializedViewsLoadAsync(true);
        } else {
            startViewsLoadAsync(true);
        }
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
