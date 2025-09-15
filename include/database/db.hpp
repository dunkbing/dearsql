#pragma once

#include "soci/row.h"

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

// Utility function for converting SOCI row values to strings
std::string convertRowValue(const soci::row& row, std::size_t columnIndex);
