#pragma once

#include "connection_config.hpp"
#include "db_interface.hpp"
#include <memory>

/**
 * @brief Factory for creating database connections
 *
 * This factory centralizes the creation of database connections,
 * abstracting away the specific database implementation details.
 */
class ConnectionFactory {
public:
    /**
     * @brief Create a database interface based on config
     * @param config Connection configuration
     * @return Shared pointer to database interface, or nullptr on failure
     */
    static std::shared_ptr<DatabaseInterface> create(const ConnectionConfig& config);

    /**
     * @brief Create a database interface based on legacy connection info
     * @param info Database connection information
     * @return Shared pointer to database interface, or nullptr on failure
     */
    static std::shared_ptr<DatabaseInterface> create(const DatabaseConnectionInfo& info);

    /**
     * @brief Test a connection without fully establishing it
     * @param config Connection configuration
     * @return pair<success, error_message>
     */
    static std::pair<bool, std::string> testConnection(const ConnectionConfig& config);
};
