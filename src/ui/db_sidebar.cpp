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

    // Load tables when the tree node is opened (expanded) and tables haven't been loaded yet
    if (dbOpen && !db->areTablesLoaded()) {
        std::cout << "Database expanded and tables not loaded yet, attempting to load..."
                  << std::endl;
        if (!db->isConnected() && !db->hasAttemptedConnection()) {
            std::cout << "Database not connected, attempting to connect..." << std::endl;
            auto [success, error] = db->connect();
            if (!success) {
                std::cerr << "Failed to connect: " << error << std::endl;
            }
        }
        if (db->isConnected()) {
            db->refreshTables();
        } else if (db->hasAttemptedConnection() && !db->getLastConnectionError().empty()) {
            // Set tables as loaded to prevent further attempts and show error
            db->setTablesLoaded(true);
        }
    }

    // Context menu for database
    handleDatabaseContextMenu(databaseIndex);

    if (dbOpen) {
        // Tables
        if (db->getTables().empty()) {
            if (db->hasAttemptedConnection() && !db->getLastConnectionError().empty()) {
                // Show connection error
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("  Connection failed: %s", db->getLastConnectionError().c_str());
                ImGui::PopStyleColor();
            } else if (!db->isConnected()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
                ImGui::Text("  Click to connect");
                ImGui::PopStyleColor();
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
        if (ImGui::MenuItem("Refresh")) {
            db->setTablesLoaded(false);
            db->refreshTables();
        }
        if (ImGui::MenuItem("Retry Connection") && db->hasAttemptedConnection() &&
            !db->isConnected()) {
            // Reset connection attempt state to allow retry
            db->setAttemptedConnection(false);
            db->setTablesLoaded(false);
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
