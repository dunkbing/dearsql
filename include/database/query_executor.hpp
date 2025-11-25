#pragma once

#include "db.hpp"
#include <string>

/**
 * @brief Interface for executing SQL queries with comprehensive results
 *
 * provides a unified interface for executing SQL queries across different
 * database implementations (SQLite, PostgreSQL, MySQL). The executeQueryWithResult
 * method returns a QueryResult struct that encapsulates:
 * - SELECT results with column names and data
 * - DML (INSERT/UPDATE/DELETE) affected row counts
 * - DDL execution messages
 * - Error information and execution time
 */
class IQueryExecutor {
public:
    virtual ~IQueryExecutor() = default;

    /**
     * @brief Execute a SQL query and return comprehensive results
     *
     * @param query The SQL query to execute
     * @param rowLimit Maximum number of rows to return for SELECT queries (default: 1000)
     * @return QueryResult containing execution results, errors, and metadata
     */
    virtual QueryResult executeQueryWithResult(const std::string& query, int rowLimit = 1000) = 0;
};
