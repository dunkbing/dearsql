#pragma once

#include "ui/tab/tab.hpp"
#include <memory>
#include <string>
#include <variant>
#include <vector>

class DatabaseInterface;
class PostgresSchemaNode;
class PostgresDatabaseNode;
class MySQLDatabaseNode;
class MySQLDatabase;
class SQLiteDatabase;
class ITableDataProvider;

// Database node variant type for SQL editor tabs
using DatabaseNode =
    std::variant<std::monostate, PostgresDatabaseNode*, MySQLDatabaseNode*, SQLiteDatabase*>;

// Table data provider variant type for table viewer tabs
using TableDataNode =
    std::variant<std::monostate, PostgresSchemaNode*, MySQLDatabaseNode*, SQLiteDatabase*>;

class TabManager {
public:
    TabManager() = default;
    ~TabManager() = default;

    // Tab management
    void addTab(const std::shared_ptr<Tab>& tab);
    void removeTab(const std::shared_ptr<Tab>& tab);
    void closeTab(const std::string& name);
    void closeAllTabs();

    // Tab queries
    bool hasTab(const std::string& name) const;
    bool isEmpty() const {
        return tabs.empty();
    }
    size_t getTabCount() const {
        return tabs.size();
    }

    const std::vector<std::shared_ptr<Tab>>& getTabs() const {
        return tabs;
    }

    // Tab creation helpers
    std::shared_ptr<Tab> createSQLEditorTab(const std::string& name, const DatabaseNode& dbNode,
                                            const std::string& schemaName = "");

    std::shared_ptr<Tab> createTableViewerTab(const TableDataNode& dataNode,
                                              const std::string& tableName);

    std::shared_ptr<Tab> createDiagramTab(PostgresSchemaNode* schemaNode);
    std::shared_ptr<Tab> createDiagramTab(MySQLDatabaseNode* dbNode);
    std::shared_ptr<Tab> createDiagramTab(SQLiteDatabase* dbNode);

    // UI rendering
    void renderTabs();
    static void renderEmptyState();

private:
    std::vector<std::shared_ptr<Tab>> tabs;

    std::string generateSQLEditorName() const;
};
