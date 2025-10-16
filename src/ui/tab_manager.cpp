#include "ui/tab_manager.hpp"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "ui/tab/diagram_tab.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/table_viewer_tab.hpp"
#include <algorithm>
#include <iostream>

void TabManager::addTab(const std::shared_ptr<Tab>& tab) {
    tabs.push_back(tab);
}

void TabManager::removeTab(const std::shared_ptr<Tab>& tab) {
    const auto it = std::ranges::find(tabs, tab);
    if (it != tabs.end()) {
        tabs.erase(it);
    }
}

void TabManager::closeTab(const std::string& name) {
    const auto it = std::ranges::find_if(
        tabs, [&name](const std::shared_ptr<Tab>& tab) { return tab->getName() == name; });

    if (it != tabs.end()) {
        tabs.erase(it);
    }
}

void TabManager::closeAllTabs() {
    tabs.clear();
}

std::shared_ptr<Tab> TabManager::findTab(const std::string& name) const {
    const auto it = std::ranges::find_if(
        tabs, [&name](const std::shared_ptr<Tab>& tab) { return tab->getName() == name; });

    return (it != tabs.end()) ? *it : nullptr;
}

bool TabManager::hasTab(const std::string& name) const {
    return findTab(name) != nullptr;
}

std::shared_ptr<Tab>
TabManager::createSQLEditorTab(const std::string& name,
                               const std::shared_ptr<DatabaseInterface>& database,
                               const std::string& selectedDatabaseName) {
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
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    return tab;
}

std::shared_ptr<Tab> TabManager::createTableViewerTab(PostgresSchemaNode* schemaNode,
                                                      const std::string& tableName,
                                                      const std::string& databaseName,
                                                      const std::string& schemaName) {
    if (!schemaNode) {
        std::cout << "Cannot create table viewer tab: schema node is null" << std::endl;
        return nullptr;
    }

    // Build the full table path for identification
    const std::string tableFullName = databaseName + "." + schemaName + "." + tableName;

    // Check if tab already exists
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::TABLE_VIEWER) {
            const auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
            if (tableTab && tableTab->getDatabasePath() == tableFullName) {
                // Tab already exists, mark it to be focused
                tableTab->setShouldFocus(true);
                std::cout << "Table " << tableName << " is already open, focusing existing tab"
                          << " " << tableFullName << " " << tableTab->getDatabasePath()
                          << std::endl;
                return tab;
            }
        }
    }

    // Create user-friendly tab name
    std::string tabName = tableName + " (" + databaseName + "." + schemaName + ")";

    // Create new tab
    auto tab = std::make_shared<TableViewerTab>(tabName, tableFullName, tableName, schemaNode);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    std::cout << "Created new tab for table: " << tableName << " with fullName: " << tableFullName
              << std::endl;
    return tab;
}

void TabManager::renderTabs() {
    // Render each tab as a separate dockable window
    for (auto it = tabs.begin(); it != tabs.end();) {
        const auto& tab = *it;

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
    constexpr auto text = "Connect to a database to get started";
    const float textWidth = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) / 2);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", text);
}

std::shared_ptr<Tab>
TabManager::createDiagramTab(const std::shared_ptr<DatabaseInterface>& database,
                             const std::string& targetDatabaseName) {
    if (!database) {
        std::cout << "Cannot create diagram tab: database is null" << std::endl;
        return nullptr;
    }

    // Generate a unique tab name for the diagram
    const std::string dbName =
        targetDatabaseName.empty() ? database->getName() : targetDatabaseName;
    const std::string baseName = "Diagram - " + dbName;
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    // Create the diagram tab with the target database name
    std::shared_ptr<Tab> tab = std::make_shared<DiagramTab>(tabName, database, targetDatabaseName);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    std::cout << "Created new diagram tab for database: " << database->getName() << std::endl;
    return tab;
}

std::string TabManager::generateSQLEditorName() const {
    int count = 1;
    const std::string baseName = "SQL Editor ";

    while (hasTab(baseName + std::to_string(count))) {
        count++;
    }

    return baseName + std::to_string(count);
}
