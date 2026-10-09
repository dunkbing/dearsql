#pragma once

#include <dearsql/query_result.hpp>
#include <dearsql/types.hpp>
#include <string>
#include <vector>

// row/schema types live in libdearsql
using dearsql::BOOL_FALSE_SENTINEL;
using dearsql::BOOL_TRUE_SENTINEL;
using dearsql::boolSentinelValue;
using dearsql::buildForeignKeyLookup;
using dearsql::Column;
using dearsql::ForeignKey;
using dearsql::formatByteSize;
using dearsql::Index;
using dearsql::isBoolSentinel;
using dearsql::isNullSentinel;
using dearsql::NULL_SENTINEL;
using dearsql::populateIncomingForeignKeys;
using dearsql::QueryResult;
using dearsql::Routine;
using dearsql::RoutineKind;
using dearsql::StatementResult;
using dearsql::Table;

struct Schema {
    std::string name;
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;
};

// Query builder functions (Drizzle-like API)
namespace sql {
    std::string and_(const std::vector<std::string>& conditions);
    std::string or_(const std::vector<std::string>& conditions);
    std::string eq(const std::string& column, const std::string& value);
    std::string like(const std::string& column, const std::string& pattern);
    std::string ilike(const std::string& column, const std::string& pattern);
} // namespace sql
