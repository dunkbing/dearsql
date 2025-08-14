#include "ui/tab_manager.hpp"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include <algorithm>
#include <iostream>

void TabManager::addTab(const std::shared_ptr<Tab> &tab) {
    tabs.push_back(tab);
}

void TabManager::removeTab(const std::shared_ptr<Tab> &tab) {
    const auto it = std::ranges::find(tabs, tab);
    if (it != tabs.end()) {
        tabs.erase(it);
    }
}

void TabManager::closeTab(const std::string &name) {
    const auto it = std::ranges::find_if(
        tabs, [&name](const std::shared_ptr<Tab> &tab) { return tab->getName() == name; });

    if (it != tabs.end()) {
        tabs.erase(it);
    }
}

void TabManager::closeAllTabs() {
    tabs.clear();
}

std::shared_ptr<Tab> TabManager::findTab(const std::string &name) const {
    const auto it = std::ranges::find_if(
        tabs, [&name](const std::shared_ptr<Tab> &tab) { return tab->getName() == name; });

    return (it != tabs.end()) ? *it : nullptr;
}

std::shared_ptr<Tab> TabManager::findTableTab(const std::string &databasePath,
                                              const std::string &tableName) const {
    for (auto &tab : tabs) {
        if (tab->getType() == TabType::TABLE_VIEWER) {
            const auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
            if (tableTab && tableTab->getDatabasePath() == databasePath &&
                tableTab->getTableName() == tableName) {
                return tab;
            }
        }
    }
    return nullptr;
}

bool TabManager::hasTab(const std::string &name) const {
    return findTab(name) != nullptr;
}

std::shared_ptr<Tab>
TabManager::createSQLEditorTab(const std::string &name,
                               const std::shared_ptr<DatabaseInterface> &database,
                               const std::string &selectedDatabaseName) {
    std::string tabName;
    if (name.empty()) {
        // Generate a name based on the database connection if available
        if (database) {
            std::string baseName = "SQL - " + database->getName();

            // Make sure the name is unique
            int count = 1;
            tabName = baseName;
            while (hasTab(tabName)) {
                count++;
                tabName = baseName + " (" + std::to_string(count) + ")";
            }
        } else {
            tabName = generateSQLEditorName();
        }
    } else {
        tabName = name;
    }

    std::string selectedDbName;
    if (database) {
        // Use provided selectedDatabaseName if available, otherwise get current database name
        if (!selectedDatabaseName.empty()) {
            selectedDbName = selectedDatabaseName;
        } else {
            // Get current database name as fallback
            if (database->getType() == DatabaseType::POSTGRESQL) {
                auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
                if (pgDb) {
                    selectedDbName = pgDb->getDatabaseName();
                }
            } else if (database->getType() == DatabaseType::MYSQL) {
                auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
                if (mysqlDb) {
                    selectedDbName = mysqlDb->getDatabaseName();
                }
            } else {
                selectedDbName = database->getName();
            }
        }
    }

    auto tab = std::make_shared<SQLEditorTab>(tabName, database, selectedDbName);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto &app = Application::getInstance();
    app.resetDockingLayout();

    return tab;
}

std::shared_ptr<Tab>
TabManager::createTableViewerTab(const std::shared_ptr<DatabaseInterface> &database,
                                 const std::string &tableName) {
    if (!database) {
        std::cout << "Cannot create table viewer tab: database is null" << std::endl;
        return nullptr;
    }

    const std::string databasePath = database->getConnectionString().empty()
                                         ? database->getPath()
                                         : database->getConnectionString();

    auto existingTab = findTableTab(databasePath, tableName);
    if (existingTab) {
        // Tab already exists, mark it to be focused
        existingTab->setShouldFocus(true);
        std::cout << "Table " << tableName << " is already open, focusing existing tab"
                  << std::endl;
        return existingTab;
    }

    // Create new tab with direct database reference
    auto tab = std::make_shared<TableViewerTab>(tableName, databasePath, tableName, database);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto &app = Application::getInstance();
    app.resetDockingLayout();

    std::cout << "Created new tab for table: " << tableName << std::endl;
    return tab;
}

void TabManager::renderTabs() {
    // Render each tab as a separate dockable window
    for (auto it = tabs.begin(); it != tabs.end();) {
        const auto &tab = *it;

        // Handle tab focusing by setting next window focus
        if (tab->shouldFocus()) {
            ImGui::SetNextWindowFocus();
            tab->setShouldFocus(false); // Reset flag after use
        }

        bool isOpen = tab->isOpen();

        // Create a dockable window for each tab
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                                 ImGuiWindowFlags_NoScrollbar |
                                                 ImGuiWindowFlags_NoScrollWithMouse;

        if (ImGui::Begin(tab->getName().c_str(), &isOpen, windowFlags)) {
            tab->render();
        }
        ImGui::End();

        // Update tab open state
        tab->setOpen(isOpen);

        if (!isOpen) {
            it = tabs.erase(it);
        } else {
            ++it;
        }
    }
}

void TabManager::renderEmptyState() {
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() / 2 - 40);

    // Center the text
    const char *text = "Connect to a database to get started";
    float textWidth = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) / 2);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", text);
}

std::string TabManager::generateSQLEditorName() const {
    int count = 1;
    const std::string baseName = "SQL Editor ";

    while (hasTab(baseName + std::to_string(count))) {
        count++;
    }

    return baseName + std::to_string(count);
}
