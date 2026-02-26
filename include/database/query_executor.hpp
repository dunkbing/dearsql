#pragma once

#include "db.hpp"
#include <string>

/**
 * @brief Interface for executing queries with comprehensive results
 */
class IQueryExecutor {
public:
    virtual ~IQueryExecutor() = default;

    virtual QueryResult executeQuery(const std::string& query, int rowLimit = 1000) = 0;
};
