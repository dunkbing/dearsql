#include "ui/tab_manager.hpp"
#include "application.hpp"
#include "database/mysql.hpp"
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

std::shared_ptr<Tab> TabManager::findTableTab(const std::shared_ptr<DatabaseInterface>& database,
                                              const std::string& tableName) const {
    // Use Table.fullName for precise tab identification - this handles cases where:
    // - Same table name exists in different databases within the same connection
    // - Same table name exists in different schemas (PostgreSQL)
    // - Different connection types need different identification schemes
    std::string tableFullName;

    // Search in tables
    for (const auto& table : database->getTables()) {
        if (table.name == tableName) {
            tableFullName = table.fullName;
            break;
        }
    }

    // If not found in tables, search in views
    if (tableFullName.empty()) {
        for (const auto& view : database->getViews()) {
            if (view.name == tableName) {
                tableFullName = view.fullName;
                break;
            }
        }
    }

    // If we still don't have a fullName, fall back to basic identification
    if (tableFullName.empty()) {
        for (auto& tab : tabs) {
            if (tab->getType() == TabType::TABLE_VIEWER) {
                const auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
                if (tableTab && tableTab->getServerDatabase() == database &&
                    tableTab->getTableName() == tableName) {
                    return tab;
                }
            }
        }
        return nullptr;
    }

    // Use fullName for precise tab identification
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::TABLE_VIEWER) {
            const auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
            if (tableTab && tableTab->getDatabasePath() == tableFullName) {
                return tab;
            }
        }
    }
    return nullptr;
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

std::shared_ptr<Tab>
TabManager::createTableViewerTab(const std::shared_ptr<DatabaseInterface>& database,
                                 const std::string& tableName) {
    if (!database) {
        std::cout << "Cannot create table viewer tab: database is null" << std::endl;
        return nullptr;
    }

    // Check if tab already exists using fullName-based lookup
    auto existingTab = findTableTab(database, tableName);
    if (existingTab) {
        // Tab already exists, mark it to be focused
        existingTab->setShouldFocus(true);
        std::cout << "Table " << tableName << " is already open, focusing existing tab"
                  << std::endl;
        return existingTab;
    }

    // Find the table to get its fullName for precise identification
    std::string tableFullName;

    // Search in tables first
    for (const auto& table : database->getTables()) {
        if (table.name == tableName) {
            tableFullName = table.fullName;
            break;
        }
    }

    // If not found in tables, search in views
    if (tableFullName.empty()) {
        for (const auto& view : database->getViews()) {
            if (view.name == tableName) {
                tableFullName = view.fullName;
                break;
            }
        }
    }

    // Fallback if fullName is not set yet
    if (tableFullName.empty()) {
        if (database->getType() == DatabaseType::POSTGRESQL) {
            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
            if (pgDb) {
                tableFullName =
                    database->getName() + "." + pgDb->getDatabaseName() + ".public." + tableName;
            }
        } else if (database->getType() == DatabaseType::MYSQL) {
            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
            if (mysqlDb) {
                tableFullName =
                    database->getName() + "." + mysqlDb->getDatabaseName() + "." + tableName;
            }
        } else {
            tableFullName = database->getName() + "." + tableName;
        }
    }

    // Create user-friendly tab name (just table name + database context)
    std::string contextName;
    if (database->getType() == DatabaseType::POSTGRESQL) {
        auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
        if (pgDb) {
            contextName = pgDb->getDatabaseName(); // Just the database name
        } else {
            contextName = database->getName();
        }
    } else if (database->getType() == DatabaseType::MYSQL) {
        auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
        if (mysqlDb) {
            contextName = mysqlDb->getDatabaseName(); // Just the database name
        } else {
            contextName = database->getName();
        }
    } else {
        contextName = database->getName(); // For SQLite/Redis, use connection name
    }

    std::string tabName = tableName + " (" + contextName + ")";

    // Ensure tab name is unique
    int counter = 1;
    std::string originalTabName = tabName;
    while (hasTab(tabName)) {
        counter++;
        tabName = originalTabName + " #" + std::to_string(counter);
    }

    // Create new tab using fullName as the databasePath for precise identification
    auto tab = std::make_shared<TableViewerTab>(tabName, tableFullName, tableName, database);
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
    const char* text = "Connect to a database to get started";
    float textWidth = ImGui::CalcTextSize(text).x;
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
    std::string dbName = targetDatabaseName.empty() ? database->getName() : targetDatabaseName;
    std::string baseName = "Diagram - " + dbName;
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
