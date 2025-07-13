#include "ui/db_sidebar.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "imgui.h"
#include "tabs/tab_manager.hpp"
#include <iostream>

void DatabaseSidebar::render() {
    auto &app = Application::getInstance();

    ImGui::Begin("Databases");

    if (ImGui::Button("Open Database", ImVec2(-1, 0))) {
        connectionDialog.showDialog();
    }

    // Always render the dialog to handle multi-frame interactions
    if (connectionDialog.isDialogOpen()) {
        connectionDialog.showDialog();
    }

    // Check if dialog completed and get result
    auto db = connectionDialog.getResult();
    if (db) {
        auto [success, error] = db->connect();
        if (success) {
            db->refreshTables();
            std::cout << "Adding database to list. Tables loaded: " << db->getTables().size()
                      << std::endl;
            app.addDatabase(db);
        } else {
            std::cerr << "Failed to open database: " << error << std::endl;
        }
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 6.0f));
    auto &databases = app.getDatabases();
    for (size_t i = 0; i < databases.size(); i++) {
        renderDatabaseNode(i);
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

void DatabaseSidebar::renderDatabaseNode(const size_t databaseIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    // Database node
    ImGuiTreeNodeFlags dbFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_FramePadding;
    if (app.getSelectedDatabase() == static_cast<int>(databaseIndex)) {
        dbFlags |= ImGuiTreeNodeFlags_Selected;
    }

    bool dbOpen = ImGui::TreeNodeEx(db->getName().c_str(), dbFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(static_cast<int>(databaseIndex));
        app.setSelectedTable(-1);
    }

    // Context menu for database
    handleDatabaseContextMenu(databaseIndex);

    if (dbOpen) {
        // Check if database is connected
        if (!db->isConnected() && !db->hasAttemptedConnection()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("  Click to connect");
            ImGui::PopStyleColor();
        } else if (db->hasAttemptedConnection() && !db->isConnected() &&
                   !db->getLastConnectionError().empty()) {
            // Show connection error
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("  Connection failed: %s", db->getLastConnectionError().c_str());
            ImGui::PopStyleColor();
        } else if (db->isConnected()) {
            // Show hierarchical structure based on database type
            if (db->getType() == DatabaseType::SQLITE) {
                renderSQLiteHierarchy(databaseIndex);
            } else if (db->getType() == DatabaseType::POSTGRESQL) {
                renderPostgreSQLHierarchy(databaseIndex);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::renderTableNode(size_t databaseIndex, size_t tableIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &table = db->getTables()[tableIndex];

    ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                    ImGuiTreeNodeFlags_FramePadding;
    if (app.getSelectedDatabase() == (int)databaseIndex &&
        app.getSelectedTable() == (int)tableIndex) {
        tableFlags |= ImGuiTreeNodeFlags_Selected;
    }

    ImGui::TreeNodeEx(table.name.c_str(), tableFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(databaseIndex);
        app.setSelectedTable(tableIndex);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
    }

    handleTableContextMenu(databaseIndex, tableIndex);
}

void DatabaseSidebar::handleDatabaseContextMenu(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Refresh All")) {
            db->setTablesLoaded(false);
            db->setViewsLoaded(false);
            if (db->getType() == DatabaseType::POSTGRESQL) {
                db->setSequencesLoaded(false);
            }
            db->refreshTables();
            db->refreshViews();
            if (db->getType() == DatabaseType::POSTGRESQL) {
                db->refreshSequences();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Refresh Tables")) {
            db->setTablesLoaded(false);
            db->refreshTables();
        }
        if (ImGui::MenuItem("Refresh Views")) {
            db->setViewsLoaded(false);
            db->refreshViews();
        }
        if (db->getType() == DatabaseType::POSTGRESQL && ImGui::MenuItem("Refresh Sequences")) {
            db->setSequencesLoaded(false);
            db->refreshSequences();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Retry Connection") && db->hasAttemptedConnection() &&
            !db->isConnected()) {
            // Reset connection attempt state to allow retry
            db->setAttemptedConnection(false);
            db->setTablesLoaded(false);
            db->setViewsLoaded(false);
            db->setSequencesLoaded(false);
            db->setLastConnectionError("");
        }
        if (ImGui::MenuItem("New SQL Editor")) {
            app.getTabManager()->createSQLEditorTab();
        }
        if (ImGui::MenuItem("Disconnect")) {
            db->disconnect();
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::handleTableContextMenu(size_t databaseIndex, size_t tableIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &table = db->getTables()[tableIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show table structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::renderViewNode(size_t databaseIndex, size_t viewIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &view = db->getViews()[viewIndex];

    ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_FramePadding;

    ImGui::TreeNodeEx(view.name.c_str(), viewFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(databaseIndex);
        app.setSelectedTable(-1); // Reset table selection for views
    }

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(db->getConnectionString(), view.name);
    }

    handleViewContextMenu(databaseIndex, viewIndex);
}

void DatabaseSidebar::renderSequenceNode(size_t databaseIndex, size_t sequenceIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &sequence = db->getSequences()[sequenceIndex];

    ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_FramePadding;

    ImGui::TreeNodeEx(sequence.c_str(), sequenceFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(databaseIndex);
        app.setSelectedTable(-1); // Reset table selection for sequences
    }

    handleSequenceContextMenu(databaseIndex, sequenceIndex);
}

void DatabaseSidebar::renderSQLiteHierarchy(size_t databaseIndex) {
    renderTablesSection(databaseIndex);
    renderViewsSection(databaseIndex);
}

void DatabaseSidebar::renderPostgreSQLHierarchy(size_t databaseIndex) {
    renderTablesSection(databaseIndex);
    renderViewsSection(databaseIndex);
    renderSequencesSection(databaseIndex);
}

void DatabaseSidebar::renderTablesSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                     ImGuiTreeNodeFlags_FramePadding;

    bool tablesOpen = ImGui::TreeNodeEx("Tables", tablesFlags);

    // Load tables when the tree node is opened and tables haven't been loaded yet
    if (tablesOpen && !db->areTablesLoaded()) {
        std::cout << "Tables node expanded and tables not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshTables();
    }

    if (tablesOpen) {
        if (db->getTables().empty()) {
            if (!db->areTablesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No tables found");
            }
        } else {
            for (size_t j = 0; j < db->getTables().size(); j++) {
                renderTableNode(databaseIndex, j);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::renderViewsSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                    ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                    ImGuiTreeNodeFlags_FramePadding;

    bool viewsOpen = ImGui::TreeNodeEx("Views", viewsFlags);

    // Load views when the tree node is opened and views haven't been loaded yet
    if (viewsOpen && !db->areViewsLoaded()) {
        std::cout << "Views node expanded and views not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshViews();
    }

    if (viewsOpen) {
        if (db->getViews().empty()) {
            if (!db->areViewsLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No views found");
            }
        } else {
            for (size_t j = 0; j < db->getViews().size(); j++) {
                renderViewNode(databaseIndex, j);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::renderSequencesSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    // Only show sequences for PostgreSQL
    if (db->getType() != DatabaseType::POSTGRESQL) {
        return;
    }

    ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                        ImGuiTreeNodeFlags_FramePadding;

    bool sequencesOpen = ImGui::TreeNodeEx("Sequences", sequencesFlags);

    // Load sequences when the tree node is opened and sequences haven't been loaded yet
    if (sequencesOpen && !db->areSequencesLoaded()) {
        std::cout << "Sequences node expanded and sequences not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshSequences();
    }

    if (sequencesOpen) {
        if (db->getSequences().empty()) {
            if (!db->areSequencesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No sequences found");
            }
        } else {
            for (size_t j = 0; j < db->getSequences().size(); j++) {
                renderSequenceNode(databaseIndex, j);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::handleViewContextMenu(size_t databaseIndex, size_t viewIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &view = db->getViews()[viewIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), view.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show view structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::handleSequenceContextMenu(size_t databaseIndex, size_t sequenceIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &sequence = db->getSequences()[sequenceIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Show Details")) {
            // TODO: Show sequence details in a tab
        }
        ImGui::EndPopup();
    }
}
