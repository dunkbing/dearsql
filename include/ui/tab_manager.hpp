#pragma once

#include "ui/tab/tab.hpp"
#include <memory>
#include <vector>

class DatabaseInterface;
class PostgresSchemaNode;

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
    std::shared_ptr<Tab> findTab(const std::string& name) const;
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
    std::shared_ptr<Tab> createSQLEditorTab(const std::string& name,
                                            const std::shared_ptr<DatabaseInterface>& database,
                                            const std::string& selectedDatabaseName = "");
    std::shared_ptr<Tab> createTableViewerTab(PostgresSchemaNode* schemaNode,
                                              const std::string& tableName,
                                              const std::string& databaseName,
                                              const std::string& schemaName);
    std::shared_ptr<Tab> createDiagramTab(const std::shared_ptr<DatabaseInterface>& database,
                                          const std::string& targetDatabaseName = "");

    // UI rendering
    void renderTabs();
    static void renderEmptyState();

private:
    std::vector<std::shared_ptr<Tab>> tabs;

    std::string generateSQLEditorName() const;
};
