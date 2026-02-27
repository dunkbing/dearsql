#include "ui/query_history.hpp"

QueryHistoryEntry::QueryHistoryEntry(std::string q, QueryType t, int rows, int duration,
                                     std::string dbName)
    : query(std::move(q)), type(t), timestamp(std::chrono::system_clock::now()), rowCount(rows),
      durationMs(duration), databaseName(std::move(dbName)) {}

QueryHistory& QueryHistory::instance() {
    static QueryHistory instance;
    return instance;
}

void QueryHistory::add(const std::string& query, int rowCount, int durationMs,
                       const std::string& databaseName) {
    if (query.empty()) {
        return;
    }

    std::string normalized = normalizeQuery(query);
    if (normalized.empty()) {
        return;
    }

    QueryType type = detectQueryType(normalized);

    // Add to front (newest first)
    entries.emplace(entries.begin(), normalized, type, rowCount, durationMs, databaseName);

    // Trim to max size
    if (entries.size() > maxEntries) {
        entries.resize(maxEntries);
    }
}

const std::vector<QueryHistoryEntry>& QueryHistory::getEntries() const {
    return entries;
}

void QueryHistory::clear() {
    entries.clear();
}

QueryType QueryHistory::detectQueryType(const std::string& query) {
    // Get first word (uppercase)
    std::string firstWord;
    for (const char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!firstWord.empty()) {
                break;
            }
            continue;
        }
        firstWord += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    if (firstWord == "SELECT") {
        return QueryType::SELECT;
    }
    if (firstWord == "INSERT") {
        return QueryType::INSERT;
    }
    if (firstWord == "UPDATE") {
        return QueryType::UPDATE;
    }
    if (firstWord == "DELETE") {
        return QueryType::DELETE;
    }
    if (firstWord == "CREATE") {
        return QueryType::CREATE;
    }
    if (firstWord == "ALTER") {
        return QueryType::ALTER;
    }
    if (firstWord == "DROP") {
        return QueryType::DROP;
    }

    return QueryType::OTHER;
}

std::string QueryHistory::normalizeQuery(const std::string& query) {
    std::string result;
    result.reserve(query.size());

    bool lastWasSpace = true; // start true to trim leading space
    for (const char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!lastWasSpace) {
                result += ' ';
                lastWasSpace = true;
            }
        } else {
            result += c;
            lastWasSpace = false;
        }
    }

    // Trim trailing space
    if (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    return result;
}
