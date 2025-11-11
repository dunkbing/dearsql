#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>

void PostgresDatabaseNode::checkSchemasStatusAsync() {
    // Check the schema future in PostgresDatabaseNode
    if (schemasFuture.valid() &&
        schemasFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            schemas = schemasFuture.get();
            Logger::info(
                std::format("Async schema loading completed for database {}. Found {} schemas",
                            name, schemas.size()));
            schemasLoaded = true;
            loadingSchemas = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async schema loading for database " << name << ": " << e.what()
                      << std::endl;
            schemasLoaded = true;
            loadingSchemas = false;
        }
    }
}

void PostgresDatabaseNode::startSchemasLoadAsync(bool forceRefresh, bool refreshChildren) {
    Logger::debug("startSchemasLoadAsync for database: " + name +
                  (forceRefresh ? " (force refresh)" : "") +
                  (refreshChildren ? " (refresh children)" : ""));
    if (!parentDb) {
        return;
    }

    // Don't start if already loading
    if (loadingSchemas.load()) {
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

    loadingSchemas = true;

    // Start async loading using the database's own schemasFuture
    schemasFuture = std::async(std::launch::async, [this, refreshChildren]() {
        std::vector<std::unique_ptr<PostgresSchemaNode>> result;

        // Check if we're still supposed to be loading
        if (!loadingSchemas.load()) {
            return result;
        }

        try {
            // Ensure we have a connection pool for the specific database
            const auto& pool = connectionPool;
            if (!pool) {
                // If no pool exists for this database, create one temporarily
                const std::string dbConnectionString = parentDb->buildConnectionString(name);
                initializeConnectionPool(dbConnectionString);
            }

            if (!loadingSchemas.load()) {
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
                    if (!loadingSchemas.load()) {
                        return result;
                    }
                    schemaNames.push_back(row.get<std::string>(0));
                }
            }

            Logger::debug("Found " + std::to_string(schemaNames.size()) + " schemas in database " +
                          name);

            if (schemaNames.empty() || !loadingSchemas.load()) {
                return result;
            }

            for (const auto& schemaName : schemaNames) {
                if (!loadingSchemas.load()) {
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

void PostgresDatabaseNode::initializeConnectionPool(const std::string& connStr) {
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
            session.open(soci::postgresql, connStr);
        }));
    }

    // Wait for all connections to complete
    for (auto& future : connectionFutures) {
        future.wait();
    }

    // Store in PostgresDatabaseNode
    connectionPool = std::move(pool);
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
