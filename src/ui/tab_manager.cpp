#include "ui/tab_manager.hpp"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "ui/tab/csv_editor_tab.hpp"
#include "ui/tab/diagram_tab.hpp"
#include "ui/tab/mongo_editor_tab.hpp"
#include "ui/tab/redis_editor_tab.hpp"
#include "ui/tab/redis_key_viewer_tab.hpp"
#include "ui/tab/redis_pubsub_tab.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/table_editor_tab.hpp"
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
        clearTabState((*it)->getId());
        tabs.erase(it);
    }
}

void TabManager::closeTab(const std::uint64_t id) {
    const auto it = std::ranges::find_if(
        tabs, [id](const std::shared_ptr<Tab>& tab) { return tab && tab->getId() == id; });

    if (it != tabs.end()) {
        clearTabState(id);
        tabs.erase(it);
    }
}

void TabManager::closeAllTabs() {
    tabs.clear();
    activeTabId_ = 0;
    pendingFocusTabId_ = 0;
}

void TabManager::closeTabsForDatabase(DatabaseInterface* db) {
    if (!db)
        return;

    auto* redisDb = dynamic_cast<RedisDatabase*>(db);

    std::erase_if(tabs, [db, redisDb](const std::shared_ptr<Tab>& tab) {
        IDatabaseNode* node = nullptr;
        if (auto* t = dynamic_cast<SQLEditorTab*>(tab.get()))
            node = t->getDatabaseNode();
        else if (auto* t = dynamic_cast<TableViewerTab*>(tab.get()))
            node = t->getDatabaseNode();
        else if (auto* t = dynamic_cast<TableEditorTab*>(tab.get()))
            node = t->getDatabaseNode();
        else if (auto* t = dynamic_cast<DiagramTab*>(tab.get()))
            node = t->getDatabaseNode();
        else if (auto* t = dynamic_cast<MongoEditorTab*>(tab.get()))
            node = t->getDatabaseNode();

        if (node)
            return node->ownerDatabase() == db;

        if (redisDb) {
            if (auto* t = dynamic_cast<RedisEditorTab*>(tab.get()))
                return t->getDatabase() == redisDb;
            if (auto* t = dynamic_cast<RedisKeyViewerTab*>(tab.get()))
                return t->getDatabase() == redisDb;
            if (auto* t = dynamic_cast<RedisPubSubTab*>(tab.get()))
                return t->getDatabase() == redisDb;
        }

        return false;
    });

    pruneTabState();
}

bool TabManager::hasTabId(const std::uint64_t id) const {
    const auto it = std::ranges::find_if(
        tabs, [id](const std::shared_ptr<Tab>& tab) { return tab && tab->getId() == id; });

    return it != tabs.end();
}

bool TabManager::hasTabTitle(const std::string& title) const {
    const auto it = std::ranges::find_if(
        tabs, [&title](const std::shared_ptr<Tab>& tab) { return tab && tab->getName() == title; });

    return it != tabs.end();
}

std::string TabManager::getPreferredTabWindowNameForDocking() const {
    if (const auto tab = findTabById(pendingFocusTabId_)) {
        return tab->getWindowName();
    }

    if (const auto tab = findTabById(activeTabId_)) {
        return tab->getWindowName();
    }

    return {};
}

void TabManager::preserveFocusedTabForLayoutRebuild() {
    pendingFocusTabId_ = findTabById(pendingFocusTabId_) ? pendingFocusTabId_ : activeTabId_;
}

std::shared_ptr<Tab> TabManager::findTabById(const std::uint64_t id) const {
    if (id == 0) {
        return nullptr;
    }

    const auto it = std::ranges::find_if(
        tabs, [id](const std::shared_ptr<Tab>& tab) { return tab && tab->getId() == id; });
    return (it != tabs.end()) ? *it : nullptr;
}

void TabManager::requestTabFocus(const std::uint64_t id) {
    pendingFocusTabId_ = id;
}

void TabManager::registerOpenedTab(const std::shared_ptr<Tab>& tab) {
    if (!tab) {
        return;
    }

    requestTabFocus(tab->getId());
    addTab(tab);

    auto& app = Application::getInstance();
    app.dockTabToCenter(tab->getWindowName());
}

void TabManager::clearTabState(const std::uint64_t id) {
    if (activeTabId_ == id) {
        activeTabId_ = 0;
    }

    if (pendingFocusTabId_ == id) {
        pendingFocusTabId_ = 0;
    }
}

void TabManager::pruneTabState() {
    if (activeTabId_ != 0 && !hasTabId(activeTabId_)) {
        activeTabId_ = 0;
    }

    if (pendingFocusTabId_ != 0 && !hasTabId(pendingFocusTabId_)) {
        pendingFocusTabId_ = 0;
    }
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
        while (hasTabTitle(tabName)) {
            count++;
            tabName = baseName + " (" + std::to_string(count) + ")";
        }
    }

    auto tab = std::make_shared<SQLEditorTab>(tabName, node, schemaName);
    registerOpenedTab(tab);
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
            if (tableTab && tableTab->getDatabaseNode() == node &&
                tableTab->getDatabasePath() == tableFullName) {
                requestTabFocus(tab->getId());
                std::cout << "Table " << tableName << " is already open, focusing existing tab"
                          << std::endl;
                return tab;
            }
        }
    }

    // Create new tab
    auto tab = std::make_shared<TableViewerTab>(tabName, tableFullName, tableName, node);
    registerOpenedTab(tab);
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
        const std::uint64_t tabId = tab->getId();
        const std::string& windowName = tab->getWindowName();

        // Handle tab focusing by setting next window focus
        const bool shouldFocusTab = (pendingFocusTabId_ == tabId);
        if (shouldFocusTab) {
            ImGui::SetNextWindowFocus();
        }

        bool isOpen = tab->isOpen();

        // Create a dockable window for each tab
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
        if (tab->hasUnsavedChanges()) {
            windowFlags |= ImGuiWindowFlags_UnsavedDocument;
        }

        // Docked tabs copy tab item data into LastItemData on Begin(), so right-click on
        // the tab label can open a popup reliably from here.
        const bool beginOpen = ImGui::Begin(windowName.c_str(), &isOpen, windowFlags);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            activeTabId_ = tabId;
        }
        if (shouldFocusTab) {
            pendingFocusTabId_ = 0;
        }
        ImGui::PushID(windowName.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
        ImGui::OpenPopupOnItemClick("TabContextMenu", ImGuiPopupFlags_MouseButtonRight);
        if (ImGui::BeginPopup("TabContextMenu")) {
            const bool hasOthers = tabs.size() > 1;
            const auto targetIt = std::ranges::find_if(
                tabs, [tabId](const auto& t) { return t && t->getId() == tabId; });
            const bool hasLeft = targetIt != tabs.end() && targetIt != tabs.begin();
            const bool hasRight = targetIt != tabs.end() && std::next(targetIt) != tabs.end();

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
            if (ImGui::MenuItem("Close")) {
                isOpen = false;
            }
            if (ImGui::MenuItem("Close Others", nullptr, false, hasOthers)) {
                pendingCloseAction = CloseAction::CloseOthers;
                pendingCloseTargetId_ = tabId;
            }
            if (ImGui::MenuItem("Close All", nullptr, false, !tabs.empty())) {
                pendingCloseAction = CloseAction::CloseAll;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close to the Left", nullptr, false, hasLeft)) {
                pendingCloseAction = CloseAction::CloseLeft;
                pendingCloseTargetId_ = tabId;
            }
            if (ImGui::MenuItem("Close to the Right", nullptr, false, hasRight)) {
                pendingCloseAction = CloseAction::CloseRight;
                pendingCloseTargetId_ = tabId;
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
            clearTabState(tabId);
            it = tabs.erase(it);
        } else {
            ++it;
        }
    }

    // Process pending close actions after the loop to avoid iterator invalidation
    if (pendingCloseAction != CloseAction::None) {
        switch (pendingCloseAction) {
        case CloseAction::CloseAll:
            closeAllTabs();
            break;
        case CloseAction::CloseOthers:
            std::erase_if(tabs,
                          [&](const auto& t) { return t && t->getId() != pendingCloseTargetId_; });
            requestTabFocus(pendingCloseTargetId_);
            break;
        case CloseAction::CloseLeft: {
            auto targetIt = std::ranges::find_if(
                tabs, [&](const auto& t) { return t && t->getId() == pendingCloseTargetId_; });
            if (targetIt != tabs.end()) {
                tabs.erase(tabs.begin(), targetIt);
            }
            requestTabFocus(pendingCloseTargetId_);
            break;
        }
        case CloseAction::CloseRight: {
            auto targetIt = std::ranges::find_if(
                tabs, [&](const auto& t) { return t && t->getId() == pendingCloseTargetId_; });
            if (targetIt != tabs.end()) {
                tabs.erase(targetIt + 1, tabs.end());
            }
            requestTabFocus(pendingCloseTargetId_);
            break;
        }
        default:
            break;
        }
        pruneTabState();
        pendingCloseAction = CloseAction::None;
        pendingCloseTargetId_ = 0;
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
    while (hasTabTitle(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    // Create the diagram tab
    std::shared_ptr<Tab> tab = std::make_shared<DiagramTab>(tabName, node);
    registerOpenedTab(tab);
    std::cout << "Created new diagram tab for: " << node->getFullPath() << std::endl;
    return tab;
}

std::shared_ptr<Tab> TabManager::createMongoEditorTab(MongoDBDatabaseNode* node) {
    if (!node)
        return nullptr;

    const std::string baseName = "Query - " + node->getName();
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<MongoEditorTab>(tabName, node);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createRedisCommandEditorTab(RedisDatabase* db) {
    if (!db)
        return nullptr;

    const std::string baseName = "Redis CLI";
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<RedisEditorTab>(tabName, db);
    registerOpenedTab(tab);
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
            if (keyTab && keyTab->getDatabase() == db && keyTab->getPattern() == pattern) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    const std::string displayName = (pattern == "*") ? "Browse" : pattern;
    const std::string baseName = std::format("Redis: {}", displayName);
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<RedisKeyViewerTab>(tabName, db, pattern);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createRedisPubSubTab(RedisDatabase* db) {
    if (!db)
        return nullptr;

    // reuse existing tab for same db
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::REDIS_PUBSUB) {
            const auto pubsubTab = std::dynamic_pointer_cast<RedisPubSubTab>(tab);
            if (pubsubTab && pubsubTab->getDatabase() == db) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    const std::string baseName = "Pub/Sub";
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<RedisPubSubTab>(tabName, db);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createCsvEditorTab(const std::string& filePath) {
    if (filePath.empty())
        return nullptr;

    // reuse existing tab for the same file
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::CSV_EDITOR) {
            const auto csvTab = std::dynamic_pointer_cast<CsvEditorTab>(tab);
            if (csvTab && csvTab->getFilePath() == filePath) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    const std::string filename = filePath.substr(
        filePath.find_last_of("/\\") != std::string::npos ? filePath.find_last_of("/\\") + 1 : 0);

    std::string tabName = filename;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = filename + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<CsvEditorTab>(tabName, filePath);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createTableEditorTab(IDatabaseNode* node,
                                                      const std::string& schema) {
    if (!node)
        return nullptr;
    auto tab = std::make_shared<TableEditorTab>(node, schema);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createTableEditorTab(IDatabaseNode* node, const Table& table,
                                                      const std::string& schema) {
    if (!node)
        return nullptr;
    auto tab = std::make_shared<TableEditorTab>(node, table, schema);
    registerOpenedTab(tab);
    return tab;
}

std::string TabManager::generateSQLEditorName() const {
    int count = 1;
    const std::string baseName = "SQL Editor ";

    while (hasTabTitle(baseName + std::to_string(count))) {
        count++;
    }

    return baseName + std::to_string(count);
}
