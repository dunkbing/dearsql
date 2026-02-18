#pragma once

#include "ui/tab/tab.hpp"
#include <memory>
#include <string>
#include <vector>

class IDatabaseNode;

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

    // Tab creation helpers (unified interface)
    std::shared_ptr<Tab> createSQLEditorTab(const std::string& name, IDatabaseNode* node,
                                            const std::string& schemaName = "");

    std::shared_ptr<Tab> createTableViewerTab(IDatabaseNode* node, const std::string& tableName);

    std::shared_ptr<Tab> createDiagramTab(IDatabaseNode* node);

    // UI rendering
    void renderTabs();
    static void renderEmptyState();

private:
    enum class CloseAction { None, CloseAll, CloseOthers, CloseLeft, CloseRight };

    std::vector<std::shared_ptr<Tab>> tabs;
    CloseAction pendingCloseAction = CloseAction::None;
    std::string pendingCloseTarget;

    std::string generateSQLEditorName() const;
};
