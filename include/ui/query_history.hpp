#pragma once

#include <chrono>
#include <string>
#include <vector>

enum class QueryType { SELECT, INSERT, UPDATE, DELETE, CREATE, ALTER, DROP, OTHER };

struct QueryHistoryEntry {
    std::string query;
    QueryType type = QueryType::OTHER;
    std::chrono::system_clock::time_point timestamp;
    int rowCount = 0;
    int durationMs = 0;
    std::string databaseName;

    QueryHistoryEntry() = default;
    QueryHistoryEntry(std::string q, QueryType t, int rows, int duration, std::string dbName = "");
};

class QueryHistory {
public:
    static QueryHistory& instance();

    // Add a query to history
    void add(const std::string& query, int rowCount = 0, int durationMs = 0,
             const std::string& databaseName = "");

    // Get all history entries (newest first)
    const std::vector<QueryHistoryEntry>& getEntries() const;

    // Clear all history
    void clear();

    // Get max entries limit
    size_t getMaxEntries() const {
        return maxEntries;
    }

    // Set max entries limit
    void setMaxEntries(size_t max) {
        maxEntries = max;
    }

private:
    QueryHistory() = default;
    QueryHistory(const QueryHistory&) = delete;
    QueryHistory& operator=(const QueryHistory&) = delete;

    static QueryType detectQueryType(const std::string& query);
    static std::string normalizeQuery(const std::string& query);

    std::vector<QueryHistoryEntry> entries;
    size_t maxEntries = 100;
};
