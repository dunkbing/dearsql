#include "database/postgres/postgres_schema_node.hpp"
#include "database/db.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <soci/soci.h>
#include <unordered_map>

void PostgresSchemaNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        Logger::info(std::format("Async table loading completed for schema {}. Found {} tables",
                                 name, tables.size()));
        tablesLoaded = true;
    });
}

void PostgresSchemaNode::startTablesLoadAsync(bool forceRefresh) {
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
    tablesLoader.start([this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> PostgresSchemaNode::getTablesWithColumnsAsync() {
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
            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!tablesLoader.isRunning()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
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
            const soci::rowset rs = session->prepare << sqlQuery;

            for (const auto& row : rs) {
                if (!tablesLoader.isRunning()) {
                    break;
                }

                auto tableName = row.get<std::string>(0);
                Column col;
                col.name = row.get<std::string>(1);
                col.type = row.get<std::string>(2);
                col.isNotNull = row.get<std::string>(3) == "NO";
                col.isPrimaryKey = row.get<std::string>(4) == "true";

                tableColumns[tableName].push_back(col);
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

            result.push_back(table);
            Logger::debug("Loaded table: " + tableName + " with " +
                          std::to_string(table.columns.size()) + " columns");
        }

    } catch (const soci::soci_error& e) {
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
            const soci::rowset viewRs = session->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!viewsLoader.isRunning()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
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
            const soci::rowset rs = session->prepare << sqlQuery;

            for (const auto& row : rs) {
                if (!viewsLoader.isRunning()) {
                    break;
                }

                auto viewName = row.get<std::string>(0);
                Column col;
                col.name = row.get<std::string>(1);
                col.type = row.get<std::string>(2);
                col.isNotNull = row.get<std::string>(3) == "NO";
                col.isPrimaryKey = false; // Views don't have primary keys

                viewColumns[viewName].push_back(col);
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

    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting views with columns for schema " << name << ": " << e.what()
                  << std::endl;
        lastViewsError = e.what();
    }

    return result;
}

void PostgresSchemaNode::checkSequencesStatusAsync() {
    sequencesLoader.check([this](const std::vector<std::string>& result) {
        sequences = result;
        Logger::info(std::format("Async sequence loading completed for schema {}. Found {} sequences",
                                 name, sequences.size()));
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
            const soci::rowset sequenceRs = session->prepare << sequencesQuery;
            for (const auto& row : sequenceRs) {
                if (!sequencesLoader.isRunning()) {
                    return result;
                }
                result.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(result.size()) + " sequences in schema " + name);

    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting sequences for schema " << name << ": " << e.what() << std::endl;
        lastSequencesError = e.what();
    }

    return result;
}

std::vector<std::vector<std::string>>
PostgresSchemaNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                 const std::string& whereClause) {
    if (!parentDbNode) {
        return {};
    }
    return parentDbNode->getTableData(name, tableName, limit, offset, whereClause);
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

std::string PostgresSchemaNode::executeQuery(const std::string& query) {
    if (!parentDbNode) {
        return "Error: No database connection";
    }
    return parentDbNode->executeQueryWithResult(query, 1000).errorMessage;
}
