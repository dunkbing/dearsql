#include "ui/tab_manager.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/file_database.hpp"
#include "database/redis.hpp"
#include "database/s3.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "mcp/fuzzy.hpp"
#include "ui/fonts.hpp"
#include "ui/tab/ai_chat_tab.hpp"
#include "ui/tab/csv_editor_tab.hpp"
#include "ui/tab/diagram_tab.hpp"
#ifdef DEARSQL_HAS_MONGODB
#include "ui/tab/mongo_editor_tab.hpp"
#endif
#include "ui/tab/postgres_sequence_viewer_tab.hpp"
#include "ui/tab/redis_editor_tab.hpp"
#include "ui/tab/redis_key_viewer_tab.hpp"
#include "ui/tab/redis_pubsub_tab.hpp"
#include "ui/tab/routine_viewer_tab.hpp"
#include "ui/tab/s3_browser_tab.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/sqlite_sequence_viewer_tab.hpp"
#include "ui/tab/table_editor_tab.hpp"
#include "ui/tab/table_viewer_tab.hpp"
#include "utils/menu.hpp"
#include <algorithm>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>

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

namespace {
    // the database node a tab works on, if it is a node-bound tab
    IDatabaseNode* nodeOf(const Tab& tab) {
        if (auto* t = dynamic_cast<const SQLEditorTab*>(&tab))
            return t->getDatabaseNode();
        if (auto* t = dynamic_cast<const TableViewerTab*>(&tab))
            return t->getDatabaseNode();
        if (auto* t = dynamic_cast<const TableEditorTab*>(&tab))
            return t->getDatabaseNode();
        if (auto* t = dynamic_cast<const DiagramTab*>(&tab))
            return t->getDatabaseNode();
#ifdef DEARSQL_HAS_MONGODB
        if (auto* t = dynamic_cast<const MongoEditorTab*>(&tab))
            return t->getDatabaseNode();
#endif
        if (auto* t = dynamic_cast<const PostgresSequenceViewerTab*>(&tab))
            return t->getDatabaseNode();
        if (auto* t = dynamic_cast<const RoutineViewerTab*>(&tab))
            return t->getDatabaseNode();
        return nullptr;
    }
} // namespace

void TabManager::closeTabsForNode(const IDatabaseNode* node) {
    if (!node)
        return;
    std::erase_if(tabs, [node](const std::shared_ptr<Tab>& tab) {
        for (const IDatabaseNode* n = tab ? nodeOf(*tab) : nullptr; n; n = n->parentNode()) {
            if (n == node)
                return true;
        }
        return false;
    });
    pruneTabState();
}

void TabManager::retireNode(std::unique_ptr<IDatabaseNode> node) {
    if (!node)
        return;
    // tabs first; the node itself dies on the reaper, where its destructor may
    // wait out workers still running on it
    if (!renderingTabs_) {
        closeTabsForNode(node.get());
        Reaper::dispose(std::move(node));
        return;
    }
    // a tab being rendered may still use it: keep it alive until the loop ends
    deferAfterRender([this, held = std::shared_ptr<IDatabaseNode>(std::move(node))]() mutable {
        closeTabsForNode(held.get());
        Reaper::dispose(std::move(held));
    });
}

void TabManager::closeTabsForDatabase(DatabaseInterface* db) {
    if (!db)
        return;

    auto* redisDb = dynamic_cast<RedisDatabase*>(db);
    auto* sqliteDb = dynamic_cast<FileDatabase*>(db);
    // bucket tabs keep the client they opened with: an edited connection must not reuse them
    const std::string s3Name =
        dynamic_cast<S3Database*>(db) ? db->getConnectionInfo().name : std::string();

    std::erase_if(tabs, [db, redisDb, sqliteDb, &s3Name](const std::shared_ptr<Tab>& tab) {
        if (auto* t = dynamic_cast<S3BrowserTab*>(tab.get()))
            return !s3Name.empty() && t->connection() == s3Name;

        if (auto* t = dynamic_cast<AIChatTab*>(tab.get()))
            return t->panel().database() == db;

        if (IDatabaseNode* node = nodeOf(*tab))
            return node->ownerDatabase() == db;

        if (sqliteDb) {
            if (auto* t = dynamic_cast<SQLiteSequenceViewerTab*>(tab.get()))
                return t->getDatabase() == sqliteDb;
        }

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

    std::string baseName = "SQL";
    if (const auto* schemaNode = dynamic_cast<PostgresSchemaNode*>(node)) {
        if (schemaNode->parentDbNode && schemaNode->parentDbNode->parentDb) {
            baseName += " - " + schemaNode->parentDbNode->name;
            baseName += " - " + schemaNode->name;
        }
    } else if (auto* postgresDbNode = dynamic_cast<PostgresDatabaseNode*>(node)) {
        baseName += " - " + postgresDbNode->name;
    } else if (auto* mysqlDbNode = dynamic_cast<MySQLDatabaseNode*>(node)) {
        baseName += " - " + mysqlDbNode->name;
    } else if (auto* sqliteDbNode = dynamic_cast<FileDatabase*>(node)) {
        baseName += " - " + sqliteDbNode->getName();
    } else if (auto* mssqlDbNode = dynamic_cast<MSSQLDatabaseNode*>(node)) {
        baseName += " - " + mssqlDbNode->name;
    } else if (auto* oracleDbNode = dynamic_cast<OracleDatabaseNode*>(node)) {
        baseName += " - " + oracleDbNode->name;
    } else if (auto* dbNode = node->ownerDatabase()) {
        baseName += " - " + dbNode->getConnectionInfo().host;
    } else {
        baseName += " - " + node->getName();
    }

    if (!name.empty()) {
        baseName += " - " + name;
    }

    int count = 1;
    std::string tabName = baseName;
    while (hasTabTitle(tabName)) {
        tabName = baseName + " (" + std::to_string(++count) + ")";
    }

    auto tab = std::make_shared<SQLEditorTab>(tabName, node, schemaName);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createTableViewerTab(IDatabaseNode* node, const Table& table) {
    if (!node) {
        std::cout << "Cannot create table viewer tab: node is null" << std::endl;
        return nullptr;
    }

    std::string tableFullName = node->getFullPath() + "." + table.name;
    std::string tabName = table.name + " (" + node->getName() + ")";

    for (auto& tab : tabs) {
        if (tab->getType() == TabType::TABLE_VIEWER) {
            const auto tableTab = std::dynamic_pointer_cast<TableViewerTab>(tab);
            if (tableTab && tableTab->getDatabaseNode() == node &&
                tableTab->getDatabasePath() == tableFullName) {
                requestTabFocus(tab->getId());
                std::cout << "Table " << table.name << " is already open, focusing existing tab"
                          << std::endl;
                return tab;
            }
        }
    }

    auto tab = std::make_shared<TableViewerTab>(tabName, tableFullName, table, node);
    registerOpenedTab(tab);
    std::cout << "Created new tab for table: " << table.name << " with fullName: " << tableFullName
              << std::endl;
    return tab;
}

void TabManager::renderTabs() {
    renderTabSwitcher();
    const auto& colors = Application::getInstance().getCurrentColors();

    // Square tab corners and style tabs (selected = lighter, unselected = darker)
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0, 0, 0, 0)); // transparent
    // Keep same colors when unfocused
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0, 0, 0, 0));

    renderingTabs_ = true;
    for (auto it = tabs.begin(); it != tabs.end();) {
        const auto& tab = *it;
        const std::uint64_t tabId = tab->getId();
        const std::string& windowName = tab->getWindowName();

        const bool shouldFocusTab = (pendingFocusTabId_ == tabId);
        if (shouldFocusTab) {
            ImGui::SetNextWindowFocus();
        }

        bool isOpen = tab->isOpen();

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
        if (tab->hasUnsavedChanges()) {
            windowFlags |= ImGuiWindowFlags_UnsavedDocument;
        }

        // Docked tabs copy tab item data into LastItemData on Begin(), so right-click on
        // the tab label can open a popup reliably from here.
        ImGui::PushStyleColor(ImGuiCol_Border, colors.surface0);
        // tighter top gap between tab bar and content
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(ImGui::GetStyle().WindowPadding.x, Theme::Spacing::M));
        const bool beginOpen = ImGui::Begin(windowName.c_str(), &isOpen, windowFlags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            activeTabId_ = tabId;
        }
        if (shouldFocusTab) {
            pendingFocusTabId_ = 0;
        }
        ImGui::PushID(windowName.c_str());
        ImGui::OpenPopupOnItemClick("TabContextMenu", ImGuiPopupFlags_MouseButtonRight);
        if (UIUtils::BeginMenuPopup("TabContextMenu")) {
            const bool hasOthers = tabs.size() > 1;
            const auto targetIt = std::ranges::find_if(
                tabs, [tabId](const auto& t) { return t && t->getId() == tabId; });
            const bool hasLeft = targetIt != tabs.end() && targetIt != tabs.begin();
            const bool hasRight = targetIt != tabs.end() && std::next(targetIt) != tabs.end();

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
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
        ImGui::PopID();

        if (beginOpen) {
            ImFont* tabFont = fonts::tabFont();
            if (tabFont) {
                ImGui::PushFont(tabFont);
            }
            if (!tab->hasBottomBar()) {
                // everything sized "to the rest" stops short of the bottom strip; both
                // rects are rebuilt at each Begin
                ImGuiWindow* w = ImGui::GetCurrentWindow();
                const float inset = Theme::BottomBarHeight();
                w->ContentRegionRect.Max.y -= inset;
                w->WorkRect.Max.y -= inset;
            }
            tab->render();
            if (tabFont) {
                ImGui::PopFont();
            }
        }

        ImGui::End();

        tab->setOpen(isOpen);

        if (!isOpen) {
            clearTabState(tabId);
            it = tabs.erase(it);
        } else {
            ++it;
        }
    }
    renderingTabs_ = false;

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

    // opening a tab from inside another tab's render would push onto `tabs` while
    // the loop above still holds an iterator into it
    if (!deferredActions_.empty()) {
        auto actions = std::move(deferredActions_);
        deferredActions_.clear();
        for (const auto& action : actions) {
            action();
        }
    }

    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(3);
}

void TabManager::deferAfterRender(std::function<void()> action) {
    deferredActions_.push_back(std::move(action));
}

void TabManager::renderEmptyState() {
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() / 2 - 40);

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

    const std::string baseName = "Diagram - " + node->getFullPath();
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        count++;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

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

#ifdef DEARSQL_HAS_MONGODB
    auto tab = std::make_shared<MongoEditorTab>(tabName, node);
    registerOpenedTab(tab);
    return tab;
#else
    return nullptr;
#endif
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
                                                         const std::string& pattern, int dbIndex) {
    if (!db)
        return nullptr;

    // reuse existing tab for same db + pattern + database index
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::REDIS_KEY_VIEWER) {
            const auto keyTab = std::dynamic_pointer_cast<RedisKeyViewerTab>(tab);
            if (keyTab && keyTab->getDatabase() == db && keyTab->getPattern() == pattern &&
                keyTab->getDatabaseIndex() == dbIndex) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    const std::string displayName = (pattern == "*") ? "Browse" : pattern;
    const std::string baseName = std::format("Redis db{}: {}", dbIndex, displayName);
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<RedisKeyViewerTab>(tabName, db, pattern, dbIndex);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createRedisPubSubTab(RedisDatabase* db) {
    if (!db)
        return nullptr;

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

std::shared_ptr<Tab> TabManager::createS3BrowserTab(const std::shared_ptr<S3Database>& db,
                                                    const std::string& bucket) {
    auto client = db ? db->client() : nullptr;
    if (!client)
        return nullptr;
    const std::string& connection = db->getConnectionInfo().name;
    for (auto& tab : tabs) {
        if (const auto s3Tab = std::dynamic_pointer_cast<S3BrowserTab>(tab);
            s3Tab && s3Tab->connection() == connection && s3Tab->bucket() == bucket) {
            requestTabFocus(tab->getId());
            return tab;
        }
    }
    std::string tabName = bucket;
    for (int count = 2; hasTabTitle(tabName); ++count)
        tabName = std::format("{} ({})", bucket, count);
    auto tab = std::make_shared<S3BrowserTab>(tabName, std::move(client), connection, bucket);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createRoutineViewerTab(IDatabaseNode* node,
                                                        const Routine& routine) {
    if (!node)
        return nullptr;

    // reuse an existing viewer for the same node + routine name
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::ROUTINE_VIEWER) {
            const auto routineTab = std::dynamic_pointer_cast<RoutineViewerTab>(tab);
            if (routineTab && routineTab->getDatabaseNode() == node &&
                routineTab->getRoutineSignature() == routine.signature &&
                routineTab->getRoutineKind() == routine.kind) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    auto tab = std::make_shared<RoutineViewerTab>(node, routine);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createSQLiteSequenceViewerTab(FileDatabase* db,
                                                               const std::string& sequenceName) {
    if (!db || sequenceName.empty()) {
        return nullptr;
    }

    // reuse existing tab for same db + sequence name
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::SQLITE_SEQUENCE_VIEWER) {
            const auto seqTab = std::dynamic_pointer_cast<SQLiteSequenceViewerTab>(tab);
            if (seqTab && seqTab->getDatabase() == db &&
                seqTab->getSequenceName() == sequenceName) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    auto tab = std::make_shared<SQLiteSequenceViewerTab>(db, sequenceName);
    registerOpenedTab(tab);
    return tab;
}

std::shared_ptr<Tab> TabManager::createPostgresSequenceViewerTab(PostgresSchemaNode* schema,
                                                                 const std::string& sequenceName) {
    if (!schema || sequenceName.empty()) {
        return nullptr;
    }

    for (auto& tab : tabs) {
        if (tab->getType() == TabType::POSTGRES_SEQUENCE_VIEWER) {
            const auto seqTab = std::dynamic_pointer_cast<PostgresSequenceViewerTab>(tab);
            if (seqTab && seqTab->getSchemaNode() == schema &&
                seqTab->getSequenceName() == sequenceName) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    auto tab = std::make_shared<PostgresSequenceViewerTab>(schema, sequenceName);
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

std::shared_ptr<Tab> TabManager::createSQLEditorTabFromQuery(IDatabaseNode* node,
                                                             const SqlScript& script) {
    for (auto& tab : tabs) {
        if (tab->getType() == TabType::SQL_EDITOR) {
            const auto sqlTab = std::dynamic_pointer_cast<SQLEditorTab>(tab);
            if (sqlTab && sqlTab->getScriptId() == script.id && script.id != 0) {
                requestTabFocus(tab->getId());
                return tab;
            }
            if (sqlTab && !sqlTab->getFilePath().empty() &&
                sqlTab->getFilePath() == script.filePath) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    auto tab = std::make_shared<SQLEditorTab>(script.name, node, script.schemaName);
    tab->loadFromScript(script);
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

std::shared_ptr<Tab> TabManager::createAIChatTab(const std::shared_ptr<DatabaseInterface>& db,
                                                 const AiSession* session) {
    if (session) {
        for (const auto& tab : tabs) {
            auto* chat = dynamic_cast<AIChatTab*>(tab.get());
            if (chat && chat->panel().currentSessionId() == session->id) {
                requestTabFocus(tab->getId());
                return tab;
            }
        }
    }

    const std::string baseName = "Chat - " + (db ? db->getConnectionInfo().name : "Assistant");
    std::string tabName = baseName;
    int count = 1;
    while (hasTabTitle(tabName)) {
        ++count;
        tabName = baseName + " (" + std::to_string(count) + ")";
    }

    auto tab = std::make_shared<AIChatTab>(tabName, db);
    if (session) {
        tab->panel().openSession(*session);
    }
    registerOpenedTab(tab);
    return tab;
}

void TabManager::tickChatTabs() {
    for (const auto& tab : tabs) {
        if (auto* chat = dynamic_cast<AIChatTab*>(tab.get())) {
            chat->tick();
        }
    }
}

void TabManager::forgetChatSession(const int sessionId) {
    for (const auto& tab : tabs) {
        if (auto* chat = dynamic_cast<AIChatTab*>(tab.get())) {
            chat->panel().forgetSession(sessionId);
        }
    }
}

// ctrl+tab switcher over the tabs, drawn like the app's popups. replaces imgui's window list,
// which also offers the sidebar and cannot be styled
void TabManager::renderTabSwitcher() {
    if (ImGuiContext& g = *ImGui::GetCurrentContext(); g.ConfigNavWindowingKeyNext != 0) {
        switcherNext_ = g.ConfigNavWindowingKeyNext; // ctrl+tab, or the mac equivalent
        switcherPrev_ = g.ConfigNavWindowingKeyPrev;
        g.ConfigNavWindowingKeyNext = g.ConfigNavWindowingKeyPrev = 0;
    }

    std::erase_if(recentTabs_, [this](std::uint64_t id) { return !hasTabId(id); });
    if (switcherIndex_ < 0 && activeTabId_ != 0) {
        std::erase(recentTabs_, activeTabId_);
        recentTabs_.insert(recentTabs_.begin(), activeTabId_);
    }
    for (const auto& tab : tabs)
        if (std::ranges::find(recentTabs_, tab->getId()) == recentTabs_.end())
            recentTabs_.push_back(tab->getId());

    const int n = static_cast<int>(recentTabs_.size());
    const bool next = ImGui::IsKeyChordPressed(switcherNext_);
    const bool prev = !next && ImGui::IsKeyChordPressed(switcherPrev_);
    if ((next || prev) && n > 1)
        switcherIndex_ = ((switcherIndex_ < 0 ? 0 : switcherIndex_) + (next ? 1 : n - 1)) % n;
    if (switcherIndex_ < 0)
        return;
    if (switcherIndex_ >= n) {
        switcherIndex_ = -1;
        return;
    }
    // releasing the modifier picks the highlighted tab
    if (!ImGui::IsKeyDown(static_cast<ImGuiKey>(switcherNext_ & ImGuiMod_Mask_))) {
        requestTabFocus(recentTabs_[switcherIndex_]);
        switcherIndex_ = -1;
        return;
    }

    const auto& style = ImGui::GetStyle();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(UIUtils::kMenuPadding, UIUtils::kMenuPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, style.PopupBorderSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style.PopupRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_PopupBg]);
    ImGui::PushStyleColor(ImGuiCol_Header, style.Colors[ImGuiCol_HeaderHovered]); // as if hovered
    // tooltip: always on top, never takes focus from the tab
    ImGui::Begin("##tab_switcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_Tooltip);
    for (int i = 0; i < n; ++i)
        if (const auto tab = findTabById(recentTabs_[i]))
            ImGui::Selectable(tab->getWindowName().c_str(), i == switcherIndex_);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

// cmd/ctrl+p: every table and view the sidebar has loaded, matched as you type; enter opens it
void TabManager::renderOpenQuickly() {
    constexpr const char* popupId = "##open_quickly";
    if (ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_P)) {
        openQuicklyQuery_[0] = '\0';
        openQuicklyIndex_ = 0;
        ImGui::OpenPopup(popupId);
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->WorkPos.y + vp->WorkSize.y * 0.15f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(640.0f, vp->WorkSize.x * 0.8f), 0.0f));
    if (!UIUtils::BeginMenuPopup(popupId)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));

    struct Hit {
        int score;
        IDatabaseNode* node;
        const Table* table;
        bool view;
        std::string where;
    };
    std::vector<Hit> hits;
    const std::string_view query = openQuicklyQuery_;
    for (const auto& db : Application::getInstance().getDatabases()) {
        if (!db || !db->isConnected())
            continue;
        for (IDatabaseNode* node : db->loadedNodes()) {
            // a file's path is its name already
            const std::string where =
                node == dynamic_cast<IDatabaseNode*>(db.get())
                    ? db->getConnectionInfo().name
                    : db->getConnectionInfo().name + " · " + node->getFullPath();
            auto add = [&](const std::vector<Table>& tables, bool view) {
                for (const auto& t : tables) {
                    const int s = query.empty() ? 1 : fuzzy::score(query, t.name);
                    if (s > 0)
                        hits.push_back({s, node, &t, view, where});
                }
            };
            if (node->isTablesLoaded())
                add(node->getTables(), false);
            if (node->isViewsLoaded())
                add(node->getViews(), true);
        }
    }
    std::ranges::stable_sort(hits, [](const Hit& a, const Hit& b) {
        return a.score != b.score ? a.score > b.score : a.table->name < b.table->name;
    });
    constexpr size_t kMaxHits = 50; // ponytail: no paging, typing narrows it
    if (hits.size() > kMaxHits)
        hits.resize(kMaxHits);
    const int n = static_cast<int>(hits.size());

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##open_quickly_query", "Open table or view...", openQuicklyQuery_,
                                 sizeof(openQuicklyQuery_)))
        openQuicklyIndex_ = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n > 0)
        openQuicklyIndex_ = (openQuicklyIndex_ + 1) % n;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n > 0)
        openQuicklyIndex_ = (openQuicklyIndex_ + n - 1) % n;
    openQuicklyIndex_ = std::clamp(openQuicklyIndex_, 0, std::max(0, n - 1));

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::PopStyleVar();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    int picked = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        picked = n > 0 ? openQuicklyIndex_ : -1;

    const auto& colors = Application::getInstance().getCurrentColors();
    if (n == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextUnformatted(query.empty()
                                   ? "Expand a database in the sidebar to load its tables."
                                   : "No matching table or view.");
        ImGui::PopStyleColor();
    }
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    // the row enter opens, drawn as if hovered
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered]);
    if (n > 0 && ImGui::BeginChild("##open_quickly_list",
                                   ImVec2(0, std::min(n, 12) * rowH + Theme::Spacing::S))) {
        for (int i = 0; i < n; ++i) {
            const auto& h = hits[i];
            ImGui::PushID(i);
            const bool selected = i == openQuicklyIndex_;
            if (ImGui::Selectable(
                    std::format("{} {}", h.view ? ICON_FA_EYE : ICON_FA_TABLE, h.table->name)
                        .c_str(),
                    selected))
                picked = i;
            if (selected && ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                selected && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                ImGui::SetScrollHereY();
            const float w = ImGui::CalcTextSize(h.where.c_str()).x;
            ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x * 0.4f,
                                     ImGui::GetWindowContentRegionMax().x - w));
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextUnformatted(h.where.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    if (n > 0)
        ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::PopStyleVar();
    if (picked >= 0) {
        const auto& h = hits[picked];
        const Table table = *h.table;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        createTableViewerTab(h.node, table);
        return;
    }
    ImGui::EndPopup();
}
