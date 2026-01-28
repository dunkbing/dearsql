#pragma once

#include "db_interface.hpp"
#include <string>

/**
 * @brief Configuration for database connections
 *
 * This struct holds all the information needed to establish a connection
 * to any supported database type. It's designed to be a simple data
 * container without database-specific logic.
 */
struct ConnectionConfig {
    // Connection identification
    std::string name; // Display name for the connection
    DatabaseType type = DatabaseType::SQLITE;

    // Common connection parameters
    std::string host;
    int port = 0;
    std::string database;
    std::string username;
    std::string password;

    // SQLite-specific
    std::string filePath; // Path to SQLite database file

    // PostgreSQL/MySQL-specific options
    bool showAllDatabases = false; // Show all databases vs. just the specified one

    // Redis-specific (if needed)
    int redisDb = 0;

    // Connection pool settings
    int poolSize = 5;

    /**
     * @brief Build a SOCI connection string from this config
     * @return Connection string suitable for SOCI
     */
    [[nodiscard]] std::string buildConnectionString() const;

    /**
     * @brief Convert to DatabaseConnectionInfo for compatibility
     * @return DatabaseConnectionInfo struct
     */
    [[nodiscard]] DatabaseConnectionInfo toDatabaseConnectionInfo() const;

    /**
     * @brief Create from existing DatabaseConnectionInfo
     * @param info The DatabaseConnectionInfo to convert from
     * @return ConnectionConfig with values from info
     */
    [[nodiscard]] static ConnectionConfig
    fromDatabaseConnectionInfo(const DatabaseConnectionInfo& info);
};
