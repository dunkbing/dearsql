#include "database/mysql/mysql_database_node.hpp"
#include "database/db.hpp"
#include "database/mysql.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <future>
#include <soci/mysql/soci-mysql.h>
#include <soci/soci.h>

void MySQLDatabaseNode::ensureConnectionPool() {
    if (!connectionPool && parentDb) {
        const std::string dbConnectionString = parentDb->buildConnectionString(name);
        initializeConnectionPool(dbConnectionString);
    }
}

void MySQLDatabaseNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        Logger::info(std::format("Async table loading completed for database {}. Found {} tables",
                                 name, tables.size()));
        tablesLoaded = true;
    });
}

void MySQLDatabaseNode::startTablesLoadAsync(bool forceRefresh) {
    Logger::debug("startTablesLoadAsync for db: " + name +
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or already loaded (unless force refresh)
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

    // Start async loading using AsyncOperation
    tablesLoader.start([this]() { return getTablesAsync(); });
}

std::vector<Table> MySQLDatabaseNode::getTablesAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!tablesLoader.isRunning()) {
        return result;
    }

    try {
        if (!tablesLoader.isRunning()) {
            return result;
        }
        Logger::info("getTablesAsync_getSession");
        const auto session = getSession();

        // Get table names
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = "SHOW TABLES";
        {
            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!tablesLoader.isRunning()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(tableNames.size()) + " tables in database " + name);

        if (tableNames.empty() || !tablesLoader.isRunning()) {
            return result;
        }

        // Load table details
        for (const auto& tableName : tableNames) {
            if (!tablesLoader.isRunning()) {
                break;
            }

            Table table;
            table.name = tableName;
            table.fullName = parentDb->getName() + "." + name + "." + tableName;

            // Get table columns
            const std::string columnsQuery = std::format("DESCRIBE `{}`", tableName);
            {
                const soci::rowset columnsRs = session->prepare << columnsQuery;

                for (const auto& colRow : columnsRs) {
                    if (!tablesLoader.isRunning()) {
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
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        Logger::info(std::format("Async view loading completed for database {}. Found {} views",
                                 name, views.size()));
        viewsLoaded = true;
    });
}

void MySQLDatabaseNode::startViewsLoadAsync(bool forceRefresh) {
    Logger::debug("startViewsLoadAsync for database: " + name);
    if (!parentDb) {
        return;
    }

    // Don't start if already loading or already loaded (unless force refresh)
    if (viewsLoader.isRunning() || (viewsLoaded && !forceRefresh)) {
        return;
    }

    // Clear previous results on force refresh
    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    // Start async loading using AsyncOperation
    viewsLoader.start([this]() { return getViewsForDatabaseAsync(); });
}

std::vector<Table> MySQLDatabaseNode::getViewsForDatabaseAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!viewsLoader.isRunning()) {
        return result;
    }

    try {
        // Ensure we have a connection pool for the specific database
        if (!connectionPool) {
            const std::string dbConnectionString = parentDb->buildConnectionString(name);
            initializeConnectionPool(dbConnectionString);
        }

        if (!viewsLoader.isRunning()) {
            return result;
        }

        Logger::info("getViewsForDatabaseAsync_getSession");
        const auto session = getSession();

        // Get view names
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        {
            const soci::rowset viewRs = session->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!viewsLoader.isRunning()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views in database " + name);

        if (viewNames.empty() || !viewsLoader.isRunning()) {
            return result;
        }

        // Load view details
        for (const auto& viewName : viewNames) {
            if (!viewsLoader.isRunning()) {
                break;
            }

            Table view;
            view.name = viewName;
            view.fullName = parentDb->getName() + "." + name + "." + viewName;

            // Get view columns (same as table columns for MySQL)
            const std::string columnsQuery = std::format("DESCRIBE `{}`", viewName);
            {
                const soci::rowset columnsRs = session->prepare << columnsQuery;

                for (const auto& colRow : columnsRs) {
                    if (!viewsLoader.isRunning()) {
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
        throw std::runtime_error(
            "MySQLDatabaseNode::getSession: Connection pool not available for database: " + name);
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

    constexpr size_t poolSize = 2;
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
        Logger::info("getTableData_getSession");
        const auto session = getSession();
        std::string query = std::format("SELECT * FROM `{}` ", tableName);

        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }

        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        const soci::rowset rs = session->prepare << query;

        for (const auto& row : rs) {
            std::vector<std::string> rowData;
            rowData.reserve(row.size());
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
        Logger::info("getColumnNames_getSession");
        const auto session = getSession();
        const std::string query = std::format("DESCRIBE `{}`", tableName);
        const soci::rowset rs = session->prepare << query;

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
        Logger::info("getRowCount_getSession");
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
        Logger::info("executeQuery_getSession");
        const auto session = getSession();
        *session << query;
        return "Query executed successfully";
    } catch (const soci::soci_error& e) {
        return std::format("Error: {}", e.what());
    }
}

QueryResult MySQLDatabaseNode::executeQueryWithResult(const std::string& query,
                                                      const int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        Logger::info("executeQueryWithResult_getSession");
        const auto session = getSession();
        if (!session) {
            result.success = false;
            result.errorMessage = "Failed to get database session";
            return result;
        }

        const soci::rowset rs = session->prepare << query;

        // get column names if available
        const auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                result.columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        // fetch rows (up to rowLimit)
        int rowCount = 0;
        for (const auto& row : rs) {
            if (rowCount >= rowLimit) {
                break;
            }

            std::vector<std::string> rowData;
            rowData.reserve(row.size());
            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.push_back(convertRowValue(row, i));
            }
            result.tableData.push_back(rowData);
            rowCount++;
        }

        // set message based on result
        if (!result.columnNames.empty()) {
            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            if (result.tableData.size() >= static_cast<size_t>(rowLimit)) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
        } else {
            result.message = "Query executed successfully";
        }

        result.success = true;
    } catch (const soci::soci_error& e) {
        result.success = false;
        result.errorMessage = "Database error: " + std::string(e.what());
        result.columnNames.clear();
        result.tableData.clear();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = "Error executing query: " + std::string(e.what());
        result.columnNames.clear();
        result.tableData.clear();
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    return result;
}
