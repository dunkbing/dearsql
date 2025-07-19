#pragma once

#include "tab.hpp"
#include <memory>
#include <vector>

class TabManager {
public:
    TabManager() = default;
    ~TabManager() = default;

    // Tab management
    void addTab(const std::shared_ptr<Tab> &tab);
    void removeTab(const std::shared_ptr<Tab> &tab);
    void closeTab(const std::string &name);
    void closeAllTabs();

    // Tab queries
    std::shared_ptr<Tab> findTab(const std::string &name) const;
    std::shared_ptr<Tab> findTableTab(const std::string &databasePath,
                                      const std::string &tableName) const;
    bool hasTab(const std::string &name) const;
    bool isEmpty() const {
        return tabs.empty();
    }
    size_t getTabCount() const {
        return tabs.size();
    }

    // Tab creation helpers
    std::shared_ptr<Tab> createSQLEditorTab(const std::string &name = "");
    std::shared_ptr<Tab> createTableViewerTab(const std::string &databasePath,
                                              const std::string &tableName);

    // UI rendering
    void renderTabs();
    void renderEmptyState();

private:
    std::vector<std::shared_ptr<Tab>> tabs;

    std::string generateSQLEditorName() const;
};
