#include "database/mysql/mysql_database_node.hpp"
#include "database/db.hpp"
#include "database/mysql.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <future>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>

void MySQLDatabaseNode::checkTablesStatusAsync() {
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tables = tablesFuture.get();
            Logger::info(
                std::format("Async table loading completed for database {}. Found {} tables", name,
                            tables.size()));
            tablesLoaded = true;
            loadingTables = false;
        } catch (const std::exception& e) {
            Logger::error(
                std::format("Error in async table loading for database {}: {}", name, e.what()));
            lastTablesError = e.what();
            tablesLoaded = true;
            loadingTables = false;
        }
    }
}

void MySQLDatabaseNode::startTablesLoadAsync() {
    Logger::debug("startTablesLoadAsync for database: " + name);
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingTables.load() || tablesLoaded) {
        return;
    }

    loadingTables = true;

    // Start async loading
    tablesFuture = std::async(std::launch::async, [this]() { return getTablesForDatabaseAsync(); });
}

std::vector<Table> MySQLDatabaseNode::getTablesForDatabaseAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingTables.load()) {
        return result;
    }

    try {
        // Ensure we have a connection pool for the specific database
        if (!connectionPool) {
            const std::string dbConnectionString = parentDb->buildConnectionString(name);
            initializeConnectionPool(dbConnectionString);
        }

        if (!loadingTables.load()) {
            return result;
        }

        // Get table names
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = "SHOW TABLES";
        {
            const auto session = getSession();
            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(tableNames.size()) + " tables in database " + name);

        if (tableNames.empty() || !loadingTables.load()) {
            return result;
        }

        // Load table details
        for (const auto& tableName : tableNames) {
            if (!loadingTables.load()) {
                break;
            }

            Table table;
            table.name = tableName;
            table.fullName = parentDb->getName() + "." + name + "." + tableName;

            // Get table columns
            const std::string columnsQuery = std::format("DESCRIBE `{}`", tableName);
            {
                const auto session = getSession();
                const soci::rowset columnsRs = session->prepare << columnsQuery;

                for (const auto& colRow : columnsRs) {
                    if (!loadingTables.load()) {
                        break;
                    }

                    Column col;
                    col.name = colRow.get<std::string>(0);                  // Field
                    col.type = colRow.get<std::string>(1);                  // Type
                    col.isNotNull = colRow.get<std::string>(2) == "NO";     // Null
                    col.isPrimaryKey = colRow.get<std::string>(3) == "PRI"; // Key
                    table.columns.push_back(col);
                }
            }

            result.push_back(table);
        }
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Error getting tables for database {}: {}", name, e.what()));
        lastTablesError = e.what();
    }

    return result;
}

void MySQLDatabaseNode::checkViewsStatusAsync() {
    if (viewsFuture.valid() &&
        viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            views = viewsFuture.get();
            Logger::info(std::format("Async view loading completed for database {}. Found {} views",
                                     name, views.size()));
            viewsLoaded = true;
            loadingViews = false;
        } catch (const std::exception& e) {
            Logger::error(
                std::format("Error in async view loading for database {}: {}", name, e.what()));
            lastViewsError = e.what();
            viewsLoaded = true;
            loadingViews = false;
        }
    }
}

void MySQLDatabaseNode::startViewsLoadAsync() {
    Logger::debug("startViewsLoadAsync for database: " + name);
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or loaded
    if (loadingViews.load() || viewsLoaded) {
        return;
    }

    loadingViews = true;

    // Start async loading
    viewsFuture = std::async(std::launch::async, [this]() { return getViewsForDatabaseAsync(); });
}

std::vector<Table> MySQLDatabaseNode::getViewsForDatabaseAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingViews.load()) {
        return result;
    }

    try {
        // Ensure we have a connection pool for the specific database
        if (!connectionPool) {
            const std::string dbConnectionString = parentDb->buildConnectionString(name);
            initializeConnectionPool(dbConnectionString);
        }

        if (!loadingViews.load()) {
            return result;
        }

        // Get view names
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        {
            const auto session = getSession();
            const soci::rowset viewRs = session->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in database " + name);

        if (viewNames.empty() || !loadingViews.load()) {
            return result;
        }

        // Load view details
        for (const auto& viewName : viewNames) {
            if (!loadingViews.load()) {
                break;
            }

            Table view;
            view.name = viewName;
            view.fullName = parentDb->getName() + "." + name + "." + viewName;

            // Get view columns (same as table columns for MySQL)
            const std::string columnsQuery = std::format("DESCRIBE `{}`", viewName);
            {
                const auto session = getSession();
                const soci::rowset columnsRs = session->prepare << columnsQuery;

                for (const auto& colRow : columnsRs) {
                    if (!loadingViews.load()) {
                        break;
                    }

                    Column col;
                    col.name = colRow.get<std::string>(0);              // Field
                    col.type = colRow.get<std::string>(1);              // Type
                    col.isNotNull = colRow.get<std::string>(2) == "NO"; // Null
                    view.columns.push_back(col);
                }
            }

            result.push_back(view);
        }
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Error getting views for database {}: {}", name, e.what()));
        lastViewsError = e.what();
    }

    return result;
}

std::unique_ptr<soci::session> MySQLDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error("Connection pool not available for database: " + name);
    }
    auto res = std::make_unique<soci::session>(*connectionPool);
    if (!res->is_connected()) {
        res->reconnect();
    }
    return res;
}

void MySQLDatabaseNode::initializeConnectionPool(const std::string& connStr) {
    Logger::debug(std::format("initializeConnectionPool {}", connStr));
    if (connectionPool) {
        return;
    }

    constexpr size_t poolSize = 3;
    auto pool = std::make_unique<soci::connection_pool>(poolSize);

    // Initialize connections in parallel for faster startup
    std::vector<std::future<void>> connectionFutures;
    connectionFutures.reserve(poolSize);

    for (size_t i = 0; i != poolSize; ++i) {
        connectionFutures.emplace_back(std::async(std::launch::async, [&pool, i, connStr]() {
            soci::session& session = pool->at(i);
            session.open(soci::mysql, connStr);
        }));
    }

    // Wait for all connections to complete
    for (auto& future : connectionFutures) {
        future.wait();
    }

    // Store in MySQLDatabaseNode
    connectionPool = std::move(pool);
}

std::vector<std::vector<std::string>>
MySQLDatabaseNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                const std::string& whereClause) {
    std::vector<std::vector<std::string>> result;

    try {
        const auto session = getSession();
        std::string query = std::format("SELECT * FROM `{}` ", tableName);

        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }

        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        const soci::rowset<soci::row> rs = session->prepare << query;

        for (const auto& row : rs) {
            std::vector<std::string> rowData;
            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.push_back(convertRowValue(row, i));
            }
            result.push_back(rowData);
        }
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Error getting table data for {}: {}", tableName, e.what()));
    }

    return result;
}

std::vector<std::string> MySQLDatabaseNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;

    try {
        const auto session = getSession();
        const std::string query = std::format("DESCRIBE `{}`", tableName);
        const soci::rowset<soci::row> rs = session->prepare << query;

        for (const auto& row : rs) {
            columnNames.push_back(row.get<std::string>(0)); // Field name is first column
        }
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Error getting column names for {}: {}", tableName, e.what()));
    }

    return columnNames;
}

int MySQLDatabaseNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    int count = 0;

    try {
        const auto session = getSession();
        std::string query = std::format("SELECT COUNT(*) FROM `{}`", tableName);

        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }

        *session << query, soci::into(count);
    } catch (const soci::soci_error& e) {
        Logger::error(std::format("Error getting row count for {}: {}", tableName, e.what()));
    }

    return count;
}

std::string MySQLDatabaseNode::executeQuery(const std::string& query) {
    try {
        const auto session = getSession();
        *session << query;
        return "Query executed successfully";
    } catch (const soci::soci_error& e) {
        return std::format("Error: {}", e.what());
    }
}
