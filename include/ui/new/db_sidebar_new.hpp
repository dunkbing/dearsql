#pragma once

#include "database/db_interface.hpp"
#include "ui/db_connection_dialog.hpp"
#include <memory>

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

private:
    void renderEmpty();
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
};
