#pragma once

#include "soci/row.h"

#include <atomic>
#include <functional>
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

    bool expanded = false;
};

struct Schema {
    std::string name;
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;
    bool expanded = false;
    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;
    bool tablesExpanded = false;
    bool viewsExpanded = false;
    bool sequencesExpanded = false;
};

/**
 * @brief Result of a SQL query execution
 *
 * encapsulates all possible outcomes of executing a SQL query:
 * - SELECT queries: columnNames + tableData
 * - INSERT/UPDATE/DELETE queries: affectedRows + message
 * - DDL queries: message
 * - Errors: success=false + errorMessage
 */
struct QueryResult {
    bool success = true;
    std::string errorMessage;

    // for SELECT queries
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> tableData;

    // for INSERT/UPDATE/DELETE queries
    int affectedRows = 0;

    // general info/message
    std::string message;

    // execution time in milliseconds
    long long executionTimeMs = 0;
};

// Utility function for converting SOCI row values to strings
std::string convertRowValue(const soci::row& row, std::size_t columnIndex);

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

class TableDataLoader {
public:
    using Task = std::function<void(TableDataLoadState&)>;

    // Start a new async task; returns false if a task is already running for tableName
    bool start(const std::string& tableName, Task task);
    void check(const std::string& tableName);
    void checkAll();
    [[nodiscard]] bool isLoading(const std::string& tableName) const;
    [[nodiscard]] bool isAnyLoading() const;
    [[nodiscard]] bool hasResult(const std::string& tableName) const;
    [[nodiscard]] bool hasAnyResult() const;
    [[nodiscard]] std::vector<std::vector<std::string>>
    getTableData(const std::string& tableName) const;
    [[nodiscard]] std::vector<std::vector<std::string>> getFirstAvailableTableData() const;
    [[nodiscard]] std::vector<std::string> getColumnNames(const std::string& tableName) const;
    [[nodiscard]] std::vector<std::string> getFirstAvailableColumnNames() const;
    [[nodiscard]] int getRowCount(const std::string& tableName) const;
    [[nodiscard]] int getFirstAvailableRowCount() const;
    [[nodiscard]] std::string getLastError(const std::string& tableName) const;
    void clear(const std::string& tableName);
    void clearAll();
    void cancelAllAndWait();

private:
    TableDataLoadState* findState(const std::string& tableName);
    const TableDataLoadState* findState(const std::string& tableName) const;

    std::unordered_map<std::string, TableDataLoadState> states;
};
