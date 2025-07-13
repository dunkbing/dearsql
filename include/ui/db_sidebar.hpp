#pragma once

#include "ui/db_connection_dialog.hpp"

class DatabaseSidebar {
public:
    DatabaseSidebar() = default;
    ~DatabaseSidebar() = default;

    void render();

private:
    void renderDatabaseNode(size_t databaseIndex);
    void renderTableNode(size_t databaseIndex, size_t tableIndex);
    void handleDatabaseContextMenu(size_t databaseIndex);
    void handleTableContextMenu(size_t databaseIndex, size_t tableIndex);

    // Database connection dialog
    DatabaseConnectionDialog connectionDialog;
};
