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

void PostgresDatabaseNode::startSchemasLoadAsync(bool forceRefresh) {
    Logger::debug("startSchemasLoadAsync for database: " + name +
                  (forceRefresh ? " (force refresh)" : ""));
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
    schemasFuture =
        std::async(std::launch::async, [this]() { return getSchemasForDatabaseAsync(); });
}

std::vector<std::unique_ptr<PostgresSchemaNode>>
PostgresDatabaseNode::getSchemasForDatabaseAsync() {
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
        std::cerr << "Error getting schemas for database " << name << ": " << e.what() << std::endl;
    }

    return result;
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
