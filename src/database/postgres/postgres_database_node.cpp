#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>

void PostgresDatabaseNode::checkSchemasStatusAsync() {
    schemasLoader.check([this](const std::vector<std::unique_ptr<PostgresSchemaNode>>& result) {
        schemas = std::move(const_cast<std::vector<std::unique_ptr<PostgresSchemaNode>>&>(result));
        Logger::info(std::format("Async schema loading completed for database {}. Found {} schemas",
                                 name, schemas.size()));
        schemasLoaded = true;
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

    // If force refresh, clear existing schemas and reset state
    if (forceRefresh) {
        schemas.clear();
        schemasLoaded = false;
        lastSchemasError.clear();
    }

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
                const auto session = getSession();
                const soci::rowset rs = session->prepare << sqlQuery;
                for (const auto& row : rs) {
                    if (!schemasLoader.isRunning()) {
                        return result;
                    }
                    schemaNames.push_back(row.get<std::string>(0));
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
        } catch (const soci::soci_error& e) {
            std::cerr << "Error getting schemas for database " << name << ": " << e.what()
                      << std::endl;
        }
        if (refreshChildren) {
            this->triggerChildSchemaRefresh();
        }

        return result;
    });
}

std::unique_ptr<soci::session> PostgresDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error("Connection pool not available for database: " + name);
    }
    auto res = std::make_unique<soci::session>(*connectionPool);
    if (!res->is_connected()) {
        res->reconnect();
    }
    return res;
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
    connectionPool = parentDb->initializeConnectionPool(info, poolSize);
}

QueryResult PostgresDatabaseNode::executeQueryWithResult(const std::string& query,
                                                         const int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
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

std::vector<std::vector<std::string>>
PostgresDatabaseNode::getTableData(const std::string& schemaName, const std::string& tableName,
                                   int limit, int offset, const std::string& whereClause) {
    std::vector<std::vector<std::string>> result;

    try {
        std::string query = std::format(R"(SELECT * FROM "{}"."{}")", schemaName, tableName);
        if (!whereClause.empty()) {
            query += " WHERE " + whereClause;
        }
        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        auto session = getSession();
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

std::vector<std::string> PostgresDatabaseNode::getColumnNames(const std::string& schemaName,
                                                              const std::string& tableName) {
    std::vector<std::string> result;

    try {
        const std::string query = std::format(
            "SELECT column_name FROM information_schema.columns WHERE table_schema = '{}' AND "
            "table_name = '{}' ORDER BY ordinal_position",
            schemaName, tableName);

        const auto session = getSession();
        const soci::rowset<std::string> rs = session->prepare << query;

        for (const auto& columnName : rs) {
            result.push_back(columnName);
        }
    } catch (const soci::soci_error& e) {
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

        const auto session = getSession();
        int count = 0;
        *session << query, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

void PostgresDatabaseNode::triggerChildSchemaRefresh() {
    Logger::debug(std::format("Triggering child schema refresh for database: {}", name));

    // loop through all schemas and trigger refresh for tables, views, and sequences
    for (auto& schema : schemas) {
        if (schema) {
            Logger::debug(std::format("Refreshing schema: {}", schema->name));

            // trigger refresh for tables
            schema->startTablesLoadAsync(true);

            // trigger refresh for views
            schema->startViewsLoadAsync(true);

            // trigger refresh for sequences
            schema->startSequencesLoadAsync(true);
        }
    }

    Logger::info(
        std::format("Triggered refresh for {} schemas in database {}", schemas.size(), name));
}
