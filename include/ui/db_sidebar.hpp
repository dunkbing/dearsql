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
    void renderDatabaseNode(size_t databaseIndex);
    void handleDatabaseContextMenu(size_t databaseIndex);
    void handleTableContextMenu(size_t databaseIndex, size_t tableIndex);
    static void handleViewContextMenu(size_t databaseIndex, size_t viewIndex);
    static void handleSequenceContextMenu(size_t databaseIndex, size_t sequenceIndex);

    // Database connection dialog
    DatabaseConnectionDialog connectionDialog;
    bool shouldShowConnectionDialog = false;
    std::shared_ptr<DatabaseInterface> databaseToEdit = nullptr;

    // Database deletion confirmation
    bool shouldShowDeleteConfirmation = false;
    int databaseToDelete = 0;

    // Database creation dialog
    bool shouldShowCreateDatabaseDialog = false;
    int createDatabaseForConnection = 0;
};
