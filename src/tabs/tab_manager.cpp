#include "tabs/tab_manager.hpp"
#include "imgui.h"
#include <algorithm>
#include <iostream>

void TabManager::addTab(std::shared_ptr<Tab> tab) {
    tabs.push_back(tab);
}

void TabManager::removeTab(std::shared_ptr<Tab> tab) {
    auto it = std::find(tabs.begin(), tabs.end(), tab);
    if (it != tabs.end()) {
        tabs.erase(it);
    }
}

void TabManager::closeTab(const std::string &name) {
    auto it = std::find_if(tabs.begin(), tabs.end(), [&name](const std::shared_ptr<Tab> &tab) {
        return tab->getName() == name;
    });

    if (it != tabs.end()) {
        tabs.erase(it);
    }
}

void TabManager::closeAllTabs() {
    tabs.clear();
}

std::shared_ptr<Tab> TabManager::findTab(const std::string &name) const {
    auto it = std::find_if(tabs.begin(), tabs.end(), [&name](const std::shared_ptr<Tab> &tab) {
        return tab->getName() == name;
    });

    return (it != tabs.end()) ? *it : nullptr;
}

std::shared_ptr<Tab> TabManager::findTableTab(const std::string &databasePath,
                                              const std::string &tableName) const {
    for (auto &tab : tabs) {
        if (tab->getType() == TabType::TABLE_VIEWER) {
            auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
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

std::shared_ptr<Tab> TabManager::createSQLEditorTab(const std::string &name) {
    std::string tabName = name.empty() ? generateSQLEditorName() : name;
    auto tab = std::make_shared<SQLEditorTab>(tabName);
    tab->setShouldFocus(true);
    addTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createTableViewerTab(const std::string &databasePath,
                                                      const std::string &tableName) {
    auto existingTab = findTableTab(databasePath, tableName);
    if (existingTab) {
        // Tab already exists, mark it to be focused
        existingTab->setShouldFocus(true);
        std::cout << "Table " << tableName << " is already open, focusing existing tab"
                  << std::endl;
        return existingTab;
    }

    // Create new tab
    auto tab = std::make_shared<TableViewerTab>(tableName, databasePath, tableName);
    tab->setShouldFocus(true);
    addTab(tab);
    std::cout << "Created new tab for table: " << tableName << std::endl;
    return tab;
}

void TabManager::renderTabs() {
    if (ImGui::BeginTabBar("ContentTabs")) {
        for (auto it = tabs.begin(); it != tabs.end();) {
            auto &tab = *it;

            // Handle tab focusing
            ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_None;
            if (tab->shouldFocus()) {
                tabFlags |= ImGuiTabItemFlags_SetSelected;
                tab->setShouldFocus(false); // Reset flag after use
            }

            bool isOpen = tab->isOpen();
            if (isOpen && ImGui::BeginTabItem(tab->getName().c_str(), &isOpen, tabFlags)) {
                tab->render();
                ImGui::EndTabItem();
            }

            // Update tab open state
            tab->setOpen(isOpen);

            if (!isOpen) {
                it = tabs.erase(it);
            } else {
                ++it;
            }
        }
        ImGui::EndTabBar();
    }
}

void TabManager::renderEmptyState() {
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() / 2);
    float buttonWidth = 200;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonWidth) / 2);
    if (ImGui::Button("Create First SQL Editor", ImVec2(buttonWidth, 0))) {
        createSQLEditorTab("SQL Editor 1");
    }
}

std::string TabManager::generateSQLEditorName() const {
    int count = 1;
    std::string baseName = "SQL Editor ";

    while (hasTab(baseName + std::to_string(count))) {
        count++;
    }

    return baseName + std::to_string(count);
}
