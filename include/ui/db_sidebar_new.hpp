#pragma once

#include "database/db_interface.hpp"
#include "database_node.hpp"
#include "imgui.h"
#include <memory>
#include <unordered_map>

class DatabaseSidebarNew {
public:
    DatabaseSidebarNew() = default;
    ~DatabaseSidebarNew() = default;

    void render();
    void showConnectionDialog();

    // Get or create a DatabaseHierarchy for a given database
    DatabaseHierarchy* getHierarchy(const std::shared_ptr<DatabaseInterface>& db);

private:
    void renderStructure();
    void renderHistory();
    void renderEmpty();
    float getHistoryButtonHeight() const;
    void renderHistoryToggleButton(const ImVec2& btnMin, float buttonW, float buttonH,
                                   bool drawRightBorder);
    void renderDatabaseNode(const std::shared_ptr<DatabaseInterface>& db);
    void handleDatabaseContextMenu(const std::shared_ptr<DatabaseInterface>& db);

    bool historyPanelOpen = false;

    // Cache of DatabaseHierarchy instances (keyed by raw pointer)
    std::unordered_map<DatabaseInterface*, std::unique_ptr<DatabaseHierarchy>> hierarchyCache;
};
