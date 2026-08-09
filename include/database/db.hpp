#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// sentinel prefix to distinguish real SQL NULL from the literal string "NULL"
inline constexpr std::string_view NULL_SENTINEL = "dearsql__null";

inline bool isNullSentinel(const std::string& v) {
    return v == NULL_SENTINEL;
}

// sentinels for boolean values so the UI can render checkboxes
inline constexpr std::string_view BOOL_TRUE_SENTINEL = "dearsql__bool_true";
inline constexpr std::string_view BOOL_FALSE_SENTINEL = "dearsql__bool_false";

inline bool isBoolSentinel(const std::string& v) {
    return v == BOOL_TRUE_SENTINEL || v == BOOL_FALSE_SENTINEL;
}

inline bool boolSentinelValue(const std::string& v) {
    return v == BOOL_TRUE_SENTINEL;
}

struct Column {
    std::string name;
    std::string type;
    std::string defaultValue;
    std::string comment;
    bool isPrimaryKey = false;
    bool isNotNull = false;
    bool isUnique = false;
    bool isAutoIncrement = false;
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
    std::string name;   // Simple table/view name (e.g., "users")
    std::string schema; // Schema/namespace for SQL qualification:
                        // Postgres: schema name ("public")
                        // MSSQL: schema name ("dbo")
                        // Oracle: owner/schema
                        // MySQL: database name
                        // SQLite: empty
    std::string comment;
    std::string definition; // View/materialized-view SQL body (the SELECT after `AS`).
                            // Empty for ordinary tables.
    std::string fullName;   // Fully qualified name for unique identification:
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

    // total on-disk size in bytes (table + indexes + toast); -1 if unknown
    int64_t sizeBytes = -1;
};

inline std::string formatByteSize(int64_t bytes) {
    if (bytes < 0)
        return {};
    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;
    constexpr double kTB = kGB * 1024.0;
    const double b = static_cast<double>(bytes);
    if (b < kKB)
        return std::format("{} B", bytes);
    if (b < kMB)
        return std::format("{:.1f} KB", b / kKB);
    if (b < kGB)
        return std::format("{:.1f} MB", b / kMB);
    if (b < kTB)
        return std::format("{:.2f} GB", b / kGB);
    return std::format("{:.2f} TB", b / kTB);
}

enum class RoutineKind { Function, Procedure };

struct Routine {
    std::string name;      // e.g. "my_func"
    std::string signature; // e.g. "my_func(integer, text)"
    RoutineKind kind = RoutineKind::Function;
    std::string returnType; // e.g. "integer", "void"
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

    // informational messages (e.g. SQL Server PRINT / RAISERROR <= 10 output)
    std::vector<std::string> messages;

    // client-measured phase timings (label, ms), in waterfall order.
    // populated only by backends that support it (postgres, mysql/mariadb)
    std::vector<std::pair<std::string, double>> phaseTimings;

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
