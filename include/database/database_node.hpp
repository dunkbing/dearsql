#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <string>
#include <vector>

/**
 * @brief Unified interface for all database nodes in the hierarchy
 *
 * This interface provides a common contract for database nodes across different
 * database types (PostgreSQL schemas, MySQL databases, SQLite databases).
 * It unifies query execution, schema information, and table data access.
 *
 * Hierarchy by database type:
 * - PostgreSQL: Connection -> Database -> Schema (implements IDatabaseNode)
 * - MySQL: Connection -> Database (implements IDatabaseNode)
 * - SQLite: Connection (implements IDatabaseNode)
 */
class IDatabaseNode {
public:
    virtual ~IDatabaseNode() = default;

    // ========== Identity ==========

    /**
     * @brief Get the display name of this node
     * @return Node name (e.g., "public" schema, "app_db" database)
     */
    [[nodiscard]] virtual std::string getName() const = 0;

    /**
     * @brief Get the full path for unique identification
     * @return Fully qualified path (e.g., "connection.database.schema")
     */
    [[nodiscard]] virtual std::string getFullPath() const = 0;

    /**
     * @brief Get the database type for this node
     * @return DatabaseType enum value
     */
    [[nodiscard]] virtual DatabaseType getDatabaseType() const = 0;

    // ========== Query Execution ==========

    /**
     * @brief Execute a SQL query (DDL/DML without result set)
     * @param sql The SQL statement to execute
     * @return pair<success, error_message>
     */
    virtual std::pair<bool, std::string> executeQuery(const std::string& sql) = 0;

    /**
     * @brief Execute a SQL query and return results
     * @param sql The SQL statement to execute
     * @param limit Maximum rows to return (default: 1000)
     * @return QueryResult with data or error
     */
    virtual std::vector<QueryResult> executeQueryWithResult(const std::string& sql,
                                                            int limit = 1000) = 0;

    // ========== Schema Information ==========

    /**
     * @brief Get tables in this node's scope
     * @return Reference to tables vector
     */
    [[nodiscard]] virtual std::vector<Table>& getTables() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getTables() const = 0;

    /**
     * @brief Get views in this node's scope
     * @return Reference to views vector
     */
    [[nodiscard]] virtual std::vector<Table>& getViews() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getViews() const = 0;

    /**
     * @brief Get sequences (PostgreSQL/SQLite only)
     * @return Reference to sequences vector
     */
    [[nodiscard]] virtual const std::vector<std::string>& getSequences() const {
        static const std::vector<std::string> empty;
        return empty;
    }

    // ========== Table Data Access ==========

    /**
     * @brief Get paginated table data
     * @param tableName Name of the table
     * @param limit Maximum rows to return
     * @param offset Starting row offset
     * @param whereClause Optional WHERE clause filter
     * @param orderBy Optional ORDER BY clause
     * @return Vector of rows (each row is vector of string values)
     */
    virtual std::vector<std::vector<std::string>> getTableData(const std::string& tableName,
                                                               int limit, int offset,
                                                               const std::string& whereClause = "",
                                                               const std::string& orderBy = "") = 0;

    /**
     * @brief Get column names for a table
     * @param tableName Name of the table
     * @return Vector of column names
     */
    [[nodiscard]] virtual std::vector<std::string> getColumnNames(const std::string& tableName) = 0;

    /**
     * @brief Get total row count for a table
     * @param tableName Name of the table
     * @param whereClause Optional WHERE clause filter
     * @return Total row count
     */
    [[nodiscard]] virtual int getRowCount(const std::string& tableName,
                                          const std::string& whereClause = "") = 0;

    // ========== Async Loading ==========

    /**
     * @brief Check if tables have been loaded
     * @return true if tables are loaded
     */
    [[nodiscard]] virtual bool isTablesLoaded() const = 0;

    /**
     * @brief Check if views have been loaded
     * @return true if views are loaded
     */
    [[nodiscard]] virtual bool isViewsLoaded() const = 0;

    /**
     * @brief Check if tables are currently loading
     * @return true if loading in progress
     */
    [[nodiscard]] virtual bool isLoadingTables() const = 0;

    /**
     * @brief Check if views are currently loading
     * @return true if loading in progress
     */
    [[nodiscard]] virtual bool isLoadingViews() const = 0;

    /**
     * @brief Start async table loading
     * @param force Force reload even if already loaded
     */
    virtual void startTablesLoadAsync(bool force = false) = 0;

    /**
     * @brief Start async view loading
     * @param force Force reload even if already loaded
     */
    virtual void startViewsLoadAsync(bool force = false) = 0;

    /**
     * @brief Check and update async loading status
     */
    virtual void checkLoadingStatus() = 0;

    // ========== Error Information ==========

    /**
     * @brief Get last error from table loading
     * @return Error message or empty string
     */
    [[nodiscard]] virtual const std::string& getLastTablesError() const {
        static const std::string empty;
        return empty;
    }

    /**
     * @brief Get last error from view loading
     * @return Error message or empty string
     */
    [[nodiscard]] virtual const std::string& getLastViewsError() const {
        static const std::string empty;
        return empty;
    }

    // ========== Table Operations ==========

    /**
     * @brief Create a table using the node's database connection
     * @param table Table definition
     * @return pair<success, error_message>
     */
    virtual std::pair<bool, std::string> createTable(const Table& table) {
        return {false, "Create table not supported for this database type"};
    }

    /**
     * @brief Start async refresh of a single table's metadata
     * @param tableName Name of the table to refresh
     */
    virtual void startTableRefreshAsync(const std::string& tableName) = 0;

    /**
     * @brief Check if a table is currently being refreshed
     * @param tableName Name of the table
     * @return true if refresh in progress
     */
    [[nodiscard]] virtual bool isTableRefreshing(const std::string& tableName) const = 0;

    /**
     * @brief Check and update table refresh status
     * @param tableName Name of the table
     */
    virtual void checkTableRefreshStatusAsync(const std::string& tableName) = 0;
};
