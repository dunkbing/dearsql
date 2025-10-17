#include "database/postgres/postgres_schema_node.hpp"
#include "database/db.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <unordered_map>

void PostgresSchemaNode::checkTablesStatusAsync() {
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tables = tablesFuture.get();
            Logger::info(std::format("Async table loading completed for schema {}. Found {} tables",
                                     name, tables.size()));
            tablesLoaded = true;
            loadingTables = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async table loading for schema " << name << ": " << e.what()
                      << std::endl;
            lastTablesError = e.what();
            tablesLoaded = true;
            loadingTables = false;
        }
    }
}

void PostgresSchemaNode::startTablesLoadAsync() {
    Logger::debug("startTablesLoadAsync for schema: " + name);
    if (!parentDbNode) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingTables.load() || tablesLoaded) {
        return;
    }

    loadingTables = true;
    tables.clear();

    // Start async loading
    tablesFuture = std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> PostgresSchemaNode::getTablesWithColumnsAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingTables.load()) {
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
                if (!loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(tableNames.size()) + " tables in schema " + name);

        if (tableNames.empty() || !loadingTables.load()) {
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
                if (!loadingTables.load()) {
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
            if (!loadingTables.load()) {
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
    if (viewsFuture.valid() &&
        viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            views = viewsFuture.get();
            Logger::info(std::format("Async view loading completed for schema {}. Found {} views",
                                     name, views.size()));
            viewsLoaded = true;
            loadingViews = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async view loading for schema " << name << ": " << e.what()
                      << std::endl;
            lastViewsError = e.what();
            viewsLoaded = true;
            loadingViews = false;
        }
    }
}

void PostgresSchemaNode::startViewsLoadAsync() {
    Logger::debug("startViewsLoadAsync for schema: " + name);
    if (!parentDbNode) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingViews.load() || viewsLoaded) {
        return;
    }

    loadingViews = true;
    views.clear();

    // Start async loading
    viewsFuture = std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> PostgresSchemaNode::getViewsWithColumnsAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingViews.load()) {
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
                if (!loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in schema " + name);

        if (viewNames.empty() || !loadingViews.load()) {
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
                if (!loadingViews.load()) {
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
            if (!loadingViews.load()) {
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
    if (sequencesFuture.valid() &&
        sequencesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            sequences = sequencesFuture.get();
            Logger::info(
                std::format("Async sequence loading completed for schema {}. Found {} sequences",
                            name, sequences.size()));
            sequencesLoaded = true;
            loadingSequences = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async sequence loading for schema " << name << ": " << e.what()
                      << std::endl;
            lastSequencesError = e.what();
            sequencesLoaded = true;
            loadingSequences = false;
        }
    }
}

void PostgresSchemaNode::startSequencesLoadAsync() {
    Logger::debug("startSequencesLoadAsync for schema: " + name);
    if (!parentDbNode) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingSequences.load() || sequencesLoaded) {
        return;
    }

    loadingSequences = true;
    sequences.clear();

    // Start async loading
    sequencesFuture = std::async(std::launch::async, [this]() { return getSequencesAsync(); });
}

std::vector<std::string> PostgresSchemaNode::getSequencesAsync() {
    std::vector<std::string> result;

    // Check if we're still supposed to be loading
    if (!loadingSequences.load()) {
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
                if (!loadingSequences.load()) {
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
PostgresSchemaNode::getTableData(const std::string& tableName, int limit, int offset,
                                 const std::string& whereClause) {
    std::vector<std::vector<std::string>> result;

    if (!parentDbNode) {
        return result;
    }

    try {
        std::string query = std::format("SELECT * FROM \"{}\".\"{}\"", name, tableName);
        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }
        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        auto session = parentDbNode->getSession();
        soci::rowset<soci::row> rs = session->prepare << query;

        for (const auto& row : rs) {
            std::vector<std::string> rowData;
            rowData.reserve(row.size());
            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.push_back(convertRowValue(row, i));
            }
            result.push_back(rowData);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }

    return result;
}

std::vector<std::string> PostgresSchemaNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> result;

    if (!parentDbNode) {
        return result;
    }

    try {
        const std::string query = std::format(
            "SELECT column_name FROM information_schema.columns WHERE table_schema = '{}' AND "
            "table_name = '{}' ORDER BY ordinal_position",
            name, tableName);

        auto session = parentDbNode->getSession();
        soci::rowset<std::string> rs = session->prepare << query;

        for (const auto& columnName : rs) {
            result.push_back(columnName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }

    return result;
}

int PostgresSchemaNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    if (!parentDbNode) {
        return 0;
    }

    try {
        std::string query = std::format("SELECT COUNT(*) FROM \"{}\".\"{}\"", name, tableName);
        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }

        auto session = parentDbNode->getSession();
        int count = 0;
        *session << query, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

std::string PostgresSchemaNode::executeQuery(const std::string& query) {
    if (!parentDbNode) {
        return "Error: No database connection";
    }

    try {
        auto session = parentDbNode->getSession();
        *session << query;
        return "Query executed successfully";
    } catch (const soci::soci_error& e) {
        return "Error: " + std::string(e.what());
    }
}
