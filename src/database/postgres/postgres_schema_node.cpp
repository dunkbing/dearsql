#include "database/postgres/postgres_schema_node.hpp"
#include "database/db.hpp"
#include "database/ddl_builder.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <format>
#include <iostream>
#include <libpq-fe.h>
#include <map>
#include <ranges>
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

} // namespace

void PostgresSchemaNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        Logger::info(std::format("Async table loading completed for schema {}. Found {} tables",
                                 name, tables.size()));
        tablesLoaded = true;
    });
}

void PostgresSchemaNode::startTablesLoadAsync(const bool forceRefresh) {
    Logger::debug("startTablesLoadAsync for schema: " + name +
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

        Logger::debug("Found " + std::to_string(tableNames.size()) + " tables in schema " + name);

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
            "as is_primary_key "
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
            table.columns = tableColumns[tableName];
            table.foreignKeys = tableForeignKeys[tableName];

            result.push_back(table);
            Logger::debug("Loaded table: " + tableName + " with " +
                          std::to_string(table.columns.size()) + " columns and " +
                          std::to_string(table.foreignKeys.size()) + " foreign keys");
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
        Logger::info(std::format("Async view loading completed for schema {}. Found {} views", name,
                                 views.size()));
        viewsLoaded = true;
    });
}

void PostgresSchemaNode::startViewsLoadAsync(bool forceRefresh) {
    Logger::debug("startViewsLoadAsync for schema: " + name +
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

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in schema " + name);

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
            view.columns = viewColumns[viewName];

            result.push_back(view);
            Logger::debug("Loaded view: " + viewName + " with " +
                          std::to_string(view.columns.size()) + " columns");
        }

    } catch (const std::exception& e) {
        std::cerr << "Error getting views with columns for schema " << name << ": " << e.what()
                  << std::endl;
        lastViewsError = e.what();
    }

    return result;
}

void PostgresSchemaNode::checkSequencesStatusAsync() {
    sequencesLoader.check([this](const std::vector<std::string>& result) {
        sequences = result;
        Logger::info(
            std::format("Async sequence loading completed for schema {}. Found {} sequences", name,
                        sequences.size()));
        sequencesLoaded = true;
    });
}

void PostgresSchemaNode::startSequencesLoadAsync(bool forceRefresh) {
    Logger::debug("startSequencesLoadAsync for schema: " + name +
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

        Logger::debug("Found " + std::to_string(result.size()) + " sequences in schema " + name);

    } catch (const std::exception& e) {
        std::cerr << "Error getting sequences for schema " << name << ": " << e.what() << std::endl;
        lastSequencesError = e.what();
    }

    return result;
}

void PostgresSchemaNode::startTableRefreshAsync(const std::string& tableName) {
    Logger::debug(std::format("Starting async refresh for table: {}.{}", name, tableName));

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
            Logger::info(std::format("Table {}.{} refreshed successfully", name, tableName));
        }

        // Clean up the loader
        tableRefreshLoaders.erase(tableName);
    });
}

Table PostgresSchemaNode::refreshTableAsync(const std::string& tableName) {
    Logger::debug(std::format("Refreshing table: {}.{}", name, tableName));

    Table refreshedTable;
    refreshedTable.name = tableName;

    if (!parentDbNode) {
        Logger::error("Cannot refresh table: no parent database node");
        return refreshedTable;
    }

    try {
        auto session = parentDbNode->getSession();
        PGconn* conn = session.get();

        // Get table columns
        const std::string columnsQuery =
            std::format("SELECT column_name, data_type, is_nullable, column_default "
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
        Logger::error(std::format("Error refreshing table {}.{}: {}", name, tableName, e.what()));
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

std::vector<QueryResult> PostgresSchemaNode::executeQuery(const std::string& query, int rowLimit) {
    if (!parentDbNode) {
        QueryResult result;
        result.success = false;
        result.errorMessage = "No database connection";
        return {result};
    }
    return parentDbNode->executeQuery(query, rowLimit);
}

std::pair<bool, std::string> PostgresSchemaNode::createTable(const Table& table) {
    if (!parentDbNode) {
        return {false, "No database connection"};
    }

    try {
        DDLBuilder builder(DatabaseType::POSTGRESQL);
        std::string sql = builder.createTable(table, name);

        auto results = parentDbNode->executeQuery(sql);
        bool success = !results.empty() && results[0].success;
        std::string error = results.empty() ? "No result" : results[0].errorMessage;
        if (!success) {
            return {false, error};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

std::string PostgresSchemaNode::getFullPath() const {
    if (!parentDbNode) {
        return name;
    }
    return parentDbNode->name + "." + name;
}

void PostgresSchemaNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
    checkSequencesStatusAsync();
}
