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

class Database {
public:
    Database(const std::string &name, const std::string &path);
    ~Database();

    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const {
        return connected;
    }

    // Getters
    const std::string &getName() const {
        return name;
    }
    const std::string &getPath() const {
        return path;
    }
    void *getConnection() const {
        return connection;
    }
    const std::vector<Table> &getTables() const {
        return tables;
    }
    std::vector<Table> &getTables() {
        return tables;
    }

    // Table management
    void refreshTables();
    bool areTablesLoaded() const {
        return tablesLoaded;
    }
    void setTablesLoaded(bool loaded) {
        tablesLoaded = loaded;
    }

    // UI state
    bool isExpanded() const {
        return expanded;
    }
    void setExpanded(bool exp) {
        expanded = exp;
    }

private:
    std::string name;
    std::string path;
    void *connection = nullptr;
    std::vector<Table> tables;
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;

    // Helper methods
    std::vector<std::string> getTableNames();
    std::vector<Column> getTableColumns(const std::string &tableName);
};
