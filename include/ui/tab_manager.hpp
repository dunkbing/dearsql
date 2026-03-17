#pragma once

#include "ui/tab/tab.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IDatabaseNode;
class MongoDBDatabaseNode;
class RedisDatabase;
class DatabaseInterface;

class TabManager {
public:
    TabManager() = default;
    ~TabManager() = default;

    // Tab management
    void addTab(const std::shared_ptr<Tab>& tab);
    void removeTab(const std::shared_ptr<Tab>& tab);
    void closeTab(std::uint64_t id);
    void closeAllTabs();
    void closeTabsForDatabase(DatabaseInterface* db);

    // Tab queries
    [[nodiscard]] bool hasTabId(std::uint64_t id) const;
    [[nodiscard]] bool hasTabTitle(const std::string& title) const;
    bool isEmpty() const {
        return tabs.empty();
    }
    size_t getTabCount() const {
        return tabs.size();
    }
    [[nodiscard]] std::string getPreferredTabWindowNameForDocking() const;
    void preserveFocusedTabForLayoutRebuild();

    const std::vector<std::shared_ptr<Tab>>& getTabs() const {
        return tabs;
    }

    // Tab creation helpers (unified interface)
    std::shared_ptr<Tab> createSQLEditorTab(const std::string& name, IDatabaseNode* node,
                                            const std::string& schemaName = "");

    std::shared_ptr<Tab> createTableViewerTab(IDatabaseNode* node, const std::string& tableName);

    std::shared_ptr<Tab> createDiagramTab(IDatabaseNode* node);

    std::shared_ptr<Tab> createMongoEditorTab(MongoDBDatabaseNode* node);

    std::shared_ptr<Tab> createRedisCommandEditorTab(RedisDatabase* db);
    std::shared_ptr<Tab> createRedisKeyViewerTab(RedisDatabase* db, const std::string& pattern);
    std::shared_ptr<Tab> createRedisPubSubTab(RedisDatabase* db);

    std::shared_ptr<Tab> createCsvEditorTab(const std::string& filePath);

    // UI rendering
    void renderTabs();
    static void renderEmptyState();

private:
    enum class CloseAction { None, CloseAll, CloseOthers, CloseLeft, CloseRight };

    [[nodiscard]] std::shared_ptr<Tab> findTabById(std::uint64_t id) const;
    void requestTabFocus(std::uint64_t id);
    void registerOpenedTab(const std::shared_ptr<Tab>& tab);
    void clearTabState(std::uint64_t id);
    void pruneTabState();

    std::vector<std::shared_ptr<Tab>> tabs;
    CloseAction pendingCloseAction = CloseAction::None;
    std::uint64_t pendingCloseTargetId_ = 0;
    std::uint64_t activeTabId_ = 0;
    std::uint64_t pendingFocusTabId_ = 0;

    std::string generateSQLEditorName() const;
};
