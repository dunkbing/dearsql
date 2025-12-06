#pragma once

#include "database/db.hpp"
#include <string>
#include <vector>

/**
 * @brief Base interface for database nodes that can provide table/view data
 *
 * This interface defines the contract for database nodes (PostgresSchemaNode, MySQLDatabaseNode)
 * to provide table and view data to UI components like TableViewerTab.
 */
class ITableDataProvider {
public:
    virtual ~ITableDataProvider() = default;

    /**
     * @brief Get paginated table/view data
     * @param tableName Name of the table or view
     * @param limit Number of rows to fetch
     * @param offset Starting row offset
     * @param whereClause Optional WHERE clause for filtering
     * @param orderByClause Optional ORDER BY clause for sorting (e.g., "column_name ASC")
     * @return Vector of rows, where each row is a vector of string values
     */
    virtual std::vector<std::vector<std::string>>
    getTableData(const std::string& tableName, int limit, int offset,
                 const std::string& whereClause = "", const std::string& orderByClause = "") = 0;

    /**
     * @brief Get column names for a table/view
     * @param tableName Name of the table or view
     * @return Vector of column names in order
     */
    virtual std::vector<std::string> getColumnNames(const std::string& tableName) = 0;

    /**
     * @brief Get total row count for a table/view
     * @param tableName Name of the table or view
     * @param whereClause Optional WHERE clause for filtering
     * @return Total number of rows
     */
    virtual int getRowCount(const std::string& tableName, const std::string& whereClause = "") = 0;

    /**
     * @brief Get access to tables for metadata (e.g., primary keys)
     * @return Reference to tables vector
     */
    virtual const std::vector<Table>& getTables() const = 0;

    /**
     * @brief Get access to views for metadata (e.g., primary keys)
     * @return Reference to views vector
     */
    virtual const std::vector<Table>& getViews() const = 0;
};
