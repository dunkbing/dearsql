#pragma once

#include "database/db_interface.hpp"
#include "ui/db_connection_dialog.hpp"
#include <memory>

class DatabaseSidebar {
public:
    DatabaseSidebar() = default;
    ~DatabaseSidebar() = default;

    void render();
    void showConnectionDialog();

private:
    void renderDatabaseNode(const std::shared_ptr<DatabaseInterface>& db);
    void handleDatabaseContextMenu(const std::shared_ptr<DatabaseInterface>& db);

    // Database connection dialog
    DatabaseConnectionDialog connectionDialog;
    bool shouldShowConnectionDialog = false;
    std::shared_ptr<DatabaseInterface> databaseToEdit = nullptr;

    // Database deletion confirmation
    bool shouldShowDeleteConfirmation = false;
    std::shared_ptr<DatabaseInterface> databasePendingDeletion = nullptr;

    // Database creation dialog
    bool shouldShowCreateDatabaseDialog = false;
    std::shared_ptr<DatabaseInterface> createDatabaseTarget = nullptr;
};
