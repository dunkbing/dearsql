#pragma once

#include <string>
#include <vector>

struct Column {
    std::string name;
    std::string type;
    std::string comment;
    bool isPrimaryKey = false;
    bool isNotNull = false;
};

struct Table {
    std::string name;     // Simple table/view name (e.g., "users")
    std::string fullName; // Fully qualified name for unique identification:
                          // SQLite: "connection.table"
                          // PostgreSQL: "connection.database.schema.table"
                          // MySQL: "connection.database.table"
                          // Redis: "connection.pattern"
    std::vector<Column> columns;
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
