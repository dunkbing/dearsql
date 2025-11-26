#include "ui/tab_manager.hpp"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "database/sqlite.hpp"
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

bool TabManager::hasTab(const std::string& name) const {
    const auto it = std::ranges::find_if(
        tabs, [&name](const std::shared_ptr<Tab>& tab) { return tab->getName() == name; });

    auto const tab = (it != tabs.end()) ? *it : nullptr;
    return tab != nullptr;
}

std::shared_ptr<Tab> TabManager::createSQLEditorTab(const std::string& name,
                                                    const DatabaseNode& dbNode,
                                                    const std::string& schemaName) {
    if (std::holds_alternative<std::monostate>(dbNode)) {
        return nullptr;
    }

    // Generate unique tab name
    std::string tabName = name;
    if (tabName.empty()) {
        std::string baseName = "SQL Editor";
        if (auto* pgNode = std::get_if<PostgresDatabaseNode*>(&dbNode); pgNode && *pgNode) {
            baseName = "SQL - " + (*pgNode)->name;
        } else if (auto* mysqlNode = std::get_if<MySQLDatabaseNode*>(&dbNode);
                   mysqlNode && *mysqlNode) {
            baseName = "SQL - " + (*mysqlNode)->name;
        } else if (auto* sqliteDb = std::get_if<SQLiteDatabase*>(&dbNode); sqliteDb && *sqliteDb) {
            baseName = "SQL - " + (*sqliteDb)->getPath();
        }

        int count = 1;
        tabName = baseName;
        while (hasTab(tabName)) {
            count++;
            tabName = baseName + " (" + std::to_string(count) + ")";
        }
    }

    auto tab = std::make_shared<SQLEditorTab>(tabName, dbNode, schemaName);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    return tab;
}

std::shared_ptr<Tab> TabManager::createTableViewerTab(const TableDataNode& dataNode,
                                                      const std::string& tableName) {
    if (std::holds_alternative<std::monostate>(dataNode)) {
        std::cout << "Cannot create table viewer tab: data node is null" << std::endl;
        return nullptr;
    }

    // Build the full table path and tab name based on node type
    std::string tableFullName;
    std::string tabName;

    if (auto* schemaNode = std::get_if<PostgresSchemaNode*>(&dataNode); schemaNode && *schemaNode) {
        const auto& dbName = (*schemaNode)->parentDbNode->name;
        const auto& schemaName = (*schemaNode)->name;
        tableFullName = dbName + "." + schemaName + "." + tableName;
        tabName = tableName + " (" + dbName + "." + schemaName + ")";
    } else if (auto* mysqlNode = std::get_if<MySQLDatabaseNode*>(&dataNode);
               mysqlNode && *mysqlNode) {
        const auto& connName = (*mysqlNode)->parentDb->getConnectionInfo().name;
        tableFullName = connName + "." + (*mysqlNode)->name + "." + tableName;
        tabName = tableName + " (" + (*mysqlNode)->name + ")";
    } else if (auto* sqliteDb = std::get_if<SQLiteDatabase*>(&dataNode); sqliteDb && *sqliteDb) {
        const auto& connName = (*sqliteDb)->getConnectionInfo().name;
        tableFullName = connName + "." + tableName;
        tabName = tableName + " (" + connName + ")";
    } else {
        std::cout << "Cannot create table viewer tab: unknown node type" << std::endl;
        return nullptr;
    }

    // Check if tab already exists
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::TABLE_VIEWER) {
            const auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
            if (tableTab && tableTab->getDatabasePath() == tableFullName) {
                tableTab->setShouldFocus(true);
                std::cout << "Table " << tableName << " is already open, focusing existing tab"
                          << std::endl;
                return tab;
            }
        }
    }

    // Create new tab
    auto tab = std::make_shared<TableViewerTab>(tabName, tableFullName, tableName, dataNode);
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

std::shared_ptr<Tab> TabManager::createDiagramTab(PostgresSchemaNode* schemaNode) {
    if (!schemaNode) {
        std::cout << "Cannot create diagram tab: schema node is null" << std::endl;
        return nullptr;
    }

    // Generate a unique tab name for the diagram
    const std::string baseName =
        "Diagram - " + schemaNode->parentDbNode->name + "." + schemaNode->name;
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    // Create the diagram tab
    std::shared_ptr<Tab> tab = std::make_shared<DiagramTab>(tabName, schemaNode);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    std::cout << "Created new diagram tab for schema: " << schemaNode->name << std::endl;
    return tab;
}

std::shared_ptr<Tab> TabManager::createDiagramTab(MySQLDatabaseNode* dbNode) {
    if (!dbNode) {
        std::cout << "Cannot create diagram tab: database node is null" << std::endl;
        return nullptr;
    }

    // Generate a unique tab name for the diagram
    const std::string baseName = "Diagram - " + dbNode->name;
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    // Create the diagram tab
    std::shared_ptr<Tab> tab = std::make_shared<DiagramTab>(tabName, dbNode);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    std::cout << "Created new diagram tab for MySQL database: " << dbNode->name << std::endl;
    return tab;
}

std::shared_ptr<Tab> TabManager::createDiagramTab(SQLiteDatabase* dbNode) {
    if (!dbNode) {
        std::cout << "Cannot create diagram tab: database node is null" << std::endl;
        return nullptr;
    }

    // Generate a unique tab name for the diagram
    const std::string baseName = "Diagram - " + dbNode->getConnectionInfo().name;
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    // Create the diagram tab
    std::shared_ptr<Tab> tab = std::make_shared<DiagramTab>(tabName, dbNode);
    tab->setShouldFocus(true);
    addTab(tab);

    // Force docking layout to be rebuilt to include the new tab
    auto& app = Application::getInstance();
    app.resetDockingLayout();

    std::cout << "Created new diagram tab for SQLite database: " << dbNode->getPath() << std::endl;
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
