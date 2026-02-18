#pragma once

#include "database/db_interface.hpp"
#include "database_node.hpp"
#include "imgui.h"
#include "ui/db_connection_dialog.hpp"
#include <memory>
#include <unordered_map>

/**
 * @brief New refactored database sidebar using per-node data attachment
 *
 * This sidebar replaces string-based lookups with direct DatabaseData/SchemaData pointers.
 * Each UI node stores its data context, eliminating the need for switchToDatabase.
 */
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

    // Connection dialog
    DatabaseConnectionDialog connectionDialog;
    bool shouldShowConnectionDialog = false;

    // Database management
    std::shared_ptr<DatabaseInterface> databasePendingDeletion;
    std::shared_ptr<DatabaseInterface> databaseToEdit;
    bool shouldShowDeleteConfirmation = false;

    // Create database dialog
    bool shouldShowCreateDatabaseDialog = false;
    std::shared_ptr<DatabaseInterface> createDatabaseTarget;

    bool historyPanelOpen = false;

    // Cache of DatabaseHierarchy instances (keyed by raw pointer for fast lookup)
    std::unordered_map<DatabaseInterface*, std::unique_ptr<DatabaseHierarchy>> hierarchyCache;
};
