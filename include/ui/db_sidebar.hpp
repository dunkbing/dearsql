#pragma once

#include "database/db_interface.hpp"
#include "database/oracle/oracle_client_installer.hpp"
#include "database_node.hpp"
#include "imgui.h"
#include <memory>
#include <unordered_map>
#include <vector>

class DatabaseSidebarNew {
public:
    DatabaseSidebarNew() = default;
    ~DatabaseSidebarNew() = default;

    void render();
    void showConnectionDialog();

    // Get or create a DatabaseHierarchy for a given database
    DatabaseHierarchy* getHierarchy(const std::shared_ptr<DatabaseInterface>& db);

    // Polls every connection's dump operations and draws their progress. Driven
    // from the frame rather than from render(), which the sidebar being hidden
    // would otherwise stop: the panel carries the only Cancel button, and without
    // the poll a finished dump never delivers its result and stays "running".
    void processDumpOperations();

    // True while any connection is running a dump. Tearing a pool down under one
    // blocks the caller until the dump finishes, so paths that disconnect need to
    // know.
    [[nodiscard]] bool hasRunningSqlDump() const;

private:
    void renderStructure();
    void renderHistory();
    void renderEmpty();
    float getHistoryButtonHeight() const;
    void renderHistoryToggleButton(const ImVec2& btnMin, float buttonW, float buttonH,
                                   bool drawRightBorder);
    void renderDatabaseNode(const std::shared_ptr<DatabaseInterface>& db);
    void handleDatabaseContextMenu(const std::shared_ptr<DatabaseInterface>& db);
    void syncHierarchyCache(const std::vector<std::shared_ptr<DatabaseInterface>>& databases);

    bool historyPanelOpen = false;
    bool texturesLoaded_ = false;

    OracleClientInstaller oracleClientInstaller_;

    // Cache of DatabaseHierarchy instances (keyed by raw pointer)
    std::unordered_map<DatabaseInterface*, std::unique_ptr<DatabaseHierarchy>> hierarchyCache;
};
