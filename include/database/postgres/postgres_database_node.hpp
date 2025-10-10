#pragma once

#include "postgres_schema_node.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <soci/connection-pool.h>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration
class PostgresDatabase;

/**
 * @brief Per-database data for PostgreSQL
 *
 * PostgreSQL hierarchy: Server → Databases → (app_db, reporting_db, ...) → Schemas
 * Each PostgresDatabaseNode represents one database within the PostgreSQL server.
 */
class PostgresDatabaseNode {
public:
    PostgresDatabase* parentDb = nullptr;

    std::string name;

    // Connection pool (one per database)
    std::unique_ptr<soci::connection_pool> connectionPool;

    // PostgreSQL: Database → Schemas → Tables/Views/Sequences
    std::vector<std::unique_ptr<PostgresSchemaNode>> schemas;
    // deprecated
    std::unordered_map<std::string, std::unique_ptr<PostgresSchemaNode>> schemaDataCache;
    bool schemasLoaded = false;
    std::atomic<bool> loadingSchemas = false;
    std::future<std::vector<std::unique_ptr<PostgresSchemaNode>>> schemasFuture;
    std::string lastSchemasError;

    // UI expansion state
    bool expanded = false;
    bool tablesExpanded = false; // For backward compatibility
    bool viewsExpanded = false;  // For backward compatibility

    // Methods
    void startSchemasLoadAsync();
    void checkSchemasStatusAsync();
    std::vector<std::unique_ptr<PostgresSchemaNode>> getSchemasForDatabaseAsync();
    std::unique_ptr<soci::session> getSession() const;
    void initializeConnectionPool(const std::string& connStr);
};
