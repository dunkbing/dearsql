#pragma once

#include "db.hpp"
#include <string>

/**
 * @brief Interface for executing queries with comprehensive results
 *
 * Returns a vector of QueryResult, each encapsulating:
 * - SELECT results with column names and data
 * - DML (INSERT/UPDATE/DELETE) affected row counts
 * - DDL execution messages
 * - Error information and execution time
 */
class IQueryExecutor {
public:
    virtual ~IQueryExecutor() = default;

    virtual std::vector<QueryResult> executeQuery(const std::string& query,
                                                  int rowLimit = 1000) = 0;
};
