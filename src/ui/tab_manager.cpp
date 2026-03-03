#include "ui/tab_manager.hpp"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "ui/tab/diagram_tab.hpp"
#include "ui/tab/redis_editor_tab.hpp"
#include "ui/tab/redis_key_viewer_tab.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/table_viewer_tab.hpp"
#include <algorithm>
#include <format>
#include <iostream>
#include <iterator>

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

std::shared_ptr<Tab> TabManager::createSQLEditorTab(const std::string& name, IDatabaseNode* node,
                                                    const std::string& schemaName) {
    if (!node) {
        return nullptr;
    }

    // Generate unique tab name
    std::string tabName = name;
    if (tabName.empty()) {
        std::string baseName = "SQL - " + node->getName();
        int count = 1;
        tabName = baseName;
        while (hasTab(tabName)) {
            count++;
            tabName = baseName + " (" + std::to_string(count) + ")";
        }
    }

    auto tab = std::make_shared<SQLEditorTab>(tabName, node, schemaName);
    tab->setShouldFocus(true);
    addTab(tab);

    // Dock the new tab into the existing center panel without rebuilding the whole layout
    auto& app = Application::getInstance();
    app.dockTabToCenter(tabName);

    return tab;
}

std::shared_ptr<Tab> TabManager::createTableViewerTab(IDatabaseNode* node,
                                                      const std::string& tableName) {
    if (!node) {
        std::cout << "Cannot create table viewer tab: node is null" << std::endl;
        return nullptr;
    }

    // Build the full table path and tab name
    std::string tableFullName = node->getFullPath() + "." + tableName;
    std::string tabName = tableName + " (" + node->getName() + ")";

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
    auto tab = std::make_shared<TableViewerTab>(tabName, tableFullName, tableName, node);
    tab->setShouldFocus(true);
    addTab(tab);

    // Dock the new tab into the existing center panel without rebuilding the whole layout
    auto& app = Application::getInstance();
    app.dockTabToCenter(tabName);

    std::cout << "Created new tab for table: " << tableName << " with fullName: " << tableFullName
              << std::endl;
    return tab;
}

void TabManager::renderTabs() {
    const auto& colors = Application::getInstance().getCurrentColors();

    // Square tab corners and style tabs (selected = lighter, unselected = darker)
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0, 0, 0, 0)); // transparent
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface0);
    // Keep same colors when unfocused
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0, 0, 0, 0));

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

        // Docked tabs copy tab item data into LastItemData on Begin(), so right-click on
        // the tab label can open a popup reliably from here.
        const bool beginOpen = ImGui::Begin(tab->getName().c_str(), &isOpen, windowFlags);
        ImGui::PushID(tab->getName().c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
        ImGui::OpenPopupOnItemClick("TabContextMenu", ImGuiPopupFlags_MouseButtonRight);
        if (ImGui::BeginPopup("TabContextMenu")) {
            const bool hasOthers = tabs.size() > 1;
            const auto targetIt = std::ranges::find_if(
                tabs, [&](const auto& t) { return t->getName() == tab->getName(); });
            const bool hasLeft = targetIt != tabs.end() && targetIt != tabs.begin();
            const bool hasRight = targetIt != tabs.end() && std::next(targetIt) != tabs.end();

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
            if (ImGui::MenuItem("Close")) {
                isOpen = false;
            }
            if (ImGui::MenuItem("Close Others", nullptr, false, hasOthers)) {
                pendingCloseAction = CloseAction::CloseOthers;
                pendingCloseTarget = tab->getName();
            }
            if (ImGui::MenuItem("Close All", nullptr, false, !tabs.empty())) {
                pendingCloseAction = CloseAction::CloseAll;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close to the Left", nullptr, false, hasLeft)) {
                pendingCloseAction = CloseAction::CloseLeft;
                pendingCloseTarget = tab->getName();
            }
            if (ImGui::MenuItem("Close to the Right", nullptr, false, hasRight)) {
                pendingCloseAction = CloseAction::CloseRight;
                pendingCloseTarget = tab->getName();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopID();

        if (beginOpen) {
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

    // Process pending close actions after the loop to avoid iterator invalidation
    if (pendingCloseAction != CloseAction::None) {
        switch (pendingCloseAction) {
        case CloseAction::CloseAll:
            tabs.clear();
            break;
        case CloseAction::CloseOthers:
            std::erase_if(tabs, [&](const auto& t) { return t->getName() != pendingCloseTarget; });
            break;
        case CloseAction::CloseLeft: {
            auto targetIt = std::ranges::find_if(
                tabs, [&](const auto& t) { return t->getName() == pendingCloseTarget; });
            if (targetIt != tabs.end()) {
                tabs.erase(tabs.begin(), targetIt);
            }
            break;
        }
        case CloseAction::CloseRight: {
            auto targetIt = std::ranges::find_if(
                tabs, [&](const auto& t) { return t->getName() == pendingCloseTarget; });
            if (targetIt != tabs.end()) {
                tabs.erase(targetIt + 1, tabs.end());
            }
            break;
        }
        default:
            break;
        }
        pendingCloseAction = CloseAction::None;
        pendingCloseTarget.clear();
    }

    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(3);
}

void TabManager::renderEmptyState() {
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() / 2 - 40);

    // Center the text
    constexpr auto text = "Connect to a database to get started";
    const float textWidth = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) / 2);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", text);
}

std::shared_ptr<Tab> TabManager::createDiagramTab(IDatabaseNode* node) {
    if (!node) {
        std::cout << "Cannot create diagram tab: node is null" << std::endl;
        return nullptr;
    }

    // Generate a unique tab name for the diagram
    const std::string baseName = "Diagram - " + node->getFullPath();
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    // Create the diagram tab
    std::shared_ptr<Tab> tab = std::make_shared<DiagramTab>(tabName, node);
    tab->setShouldFocus(true);
    addTab(tab);

    // Dock the new tab into the existing center panel without rebuilding the whole layout
    auto& app = Application::getInstance();
    app.dockTabToCenter(tabName);

    std::cout << "Created new diagram tab for: " << node->getFullPath() << std::endl;
    return tab;
}

std::shared_ptr<Tab> TabManager::createRedisCommandEditorTab(RedisDatabase* db) {
    if (!db)
        return nullptr;

    const std::string baseName = "Redis CLI";
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<RedisEditorTab>(tabName, db);
    tab->setShouldFocus(true);
    addTab(tab);

    auto& app = Application::getInstance();
    app.dockTabToCenter(tabName);

    return tab;
}

std::shared_ptr<Tab> TabManager::createRedisKeyViewerTab(RedisDatabase* db,
                                                         const std::string& pattern) {
    if (!db)
        return nullptr;

    // reuse existing tab for same db + pattern
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::REDIS_KEY_VIEWER) {
            const auto keyTab = std::dynamic_pointer_cast<RedisKeyViewerTab>(tab);
            if (keyTab && keyTab->getPattern() == pattern) {
                keyTab->setShouldFocus(true);
                return tab;
            }
        }
    }

    const std::string displayName = (pattern == "*") ? "All Keys" : pattern;
    const std::string baseName = std::format("Redis: {}", displayName);
    std::string tabName = baseName;
    int count = 1;
    while (hasTab(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<RedisKeyViewerTab>(tabName, db, pattern);
    tab->setShouldFocus(true);
    addTab(tab);

    auto& app = Application::getInstance();
    app.dockTabToCenter(tabName);

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
