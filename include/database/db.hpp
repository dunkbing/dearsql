#pragma once

#include <atomic>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

struct Column {
    std::string name;
    std::string type;
    std::string comment;
    bool isPrimaryKey = false;
    bool isNotNull = false;
};

struct Index {
    std::string name;
    std::vector<std::string> columns;
    bool isUnique = false;
    bool isPrimary = false;
    std::string type; // BTREE, HASH, etc.
};

struct ForeignKey {
    std::string name;
    std::string sourceColumn;
    std::string targetTable;
    std::string targetColumn;
    std::string onDelete; // CASCADE, SET NULL, RESTRICT, NO ACTION
    std::string onUpdate; // CASCADE, SET NULL, RESTRICT, NO ACTION
};

struct Table {
    std::string name;     // Simple table/view name (e.g., "users")
    std::string fullName; // Fully qualified name for unique identification:
                          // SQLite: "connection.table"
                          // PostgreSQL: "connection.database.schema.table"
                          // MySQL: "connection.database.table"
                          // Redis: "connection.pattern"
    std::vector<Column> columns;
    std::vector<Index> indexes;
    std::vector<ForeignKey> foreignKeys;

    // Foreign keys from other tables that reference this table
    std::vector<ForeignKey> incomingForeignKeys;

    // Fast lookup for foreign keys by source column
    std::unordered_map<std::string, ForeignKey> foreignKeysByColumn;
};

struct Schema {
    std::string name;
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;
};

/**
 * @brief Result of a single SQL statement execution
 */
struct StatementResult {
    bool success = true;
    std::string errorMessage;

    // SELECT
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> tableData;

    // INSERT/UPDATE/DELETE
    int affectedRows = 0;

    // general info/message
    std::string message;
};

/**
 * @brief Result of a query execution (may contain multiple statements)
 */
struct QueryResult {
    std::vector<StatementResult> statements;
    double executionTimeMs = 0.0;

    [[nodiscard]] bool success() const {
        if (statements.empty())
            return false;
        for (const auto& s : statements) {
            if (!s.success)
                return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& errorMessage() const {
        static const std::string empty;
        for (const auto& s : statements) {
            if (!s.success)
                return s.errorMessage;
        }
        return empty;
    }

    [[nodiscard]] bool empty() const {
        return statements.empty();
    }
    [[nodiscard]] size_t size() const {
        return statements.size();
    }

    StatementResult& operator[](size_t i) {
        return statements[i];
    }
    const StatementResult& operator[](size_t i) const {
        return statements[i];
    }
};

// Utility helpers shared across database implementations
void buildForeignKeyLookup(Table& table);
void populateIncomingForeignKeys(std::vector<Table>& tables);

// Query builder functions (Drizzle-like API)
namespace sql {
    std::string and_(const std::vector<std::string>& conditions);
    std::string or_(const std::vector<std::string>& conditions);
    std::string eq(const std::string& column, const std::string& value);
    std::string like(const std::string& column, const std::string& pattern);
    std::string ilike(const std::string& column, const std::string& pattern);
} // namespace sql

struct TableDataLoadState {
    std::atomic<bool> loading{false};
    std::atomic<bool> ready{false};
    std::vector<std::vector<std::string>> tableData;
    std::vector<std::string> columnNames;
    int rowCount = 0;
    std::string lastError;
    std::future<void> future;
};
