#pragma once

#include "ui/db_connection_dialog.hpp"

class DatabaseSidebar {
public:
    DatabaseSidebar() = default;
    ~DatabaseSidebar() = default;

    void render();
    void showConnectionDialog();

private:
    void renderDatabaseNode(size_t databaseIndex);
    void renderTableNode(int databaseIndex, int tableIndex);
    void renderViewNode(size_t databaseIndex, size_t viewIndex);
    void renderSequenceNode(size_t databaseIndex, size_t sequenceIndex);
    void renderSQLiteHierarchy(size_t databaseIndex);
    void renderPostgresHierarchy(size_t databaseIndex);
    void renderTablesSection(size_t databaseIndex);
    void renderViewsSection(size_t databaseIndex);
    void renderSequencesSection(size_t databaseIndex);
    void renderSchemasSection(size_t databaseIndex);
    void renderSchemaNode(size_t databaseIndex, size_t schemaIndex);
    void handleDatabaseContextMenu(size_t databaseIndex);
    void handleTableContextMenu(size_t databaseIndex, size_t tableIndex);
    static void handleViewContextMenu(size_t databaseIndex, size_t viewIndex);
    static void handleSequenceContextMenu(size_t databaseIndex, size_t sequenceIndex);

    // Database connection dialog
    DatabaseConnectionDialog connectionDialog;
    bool shouldShowConnectionDialog = false;

    // Database deletion confirmation
    bool shouldShowDeleteConfirmation = false;
    size_t databaseToDelete = 0;
};
