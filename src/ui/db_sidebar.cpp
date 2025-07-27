#include "ui/db_sidebar.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include "ui/mysql_hierarchy.hpp"
#include "ui/postgres_hierarchy.hpp"
#include "ui/sqlite_hierarchy.hpp"
#include "ui/tab_manager.hpp"
#include "utils/spinner.hpp"
#include <iostream>

void DatabaseSidebar::showConnectionDialog() {
    shouldShowConnectionDialog = true;
}

void DatabaseSidebar::render() {
    auto &app = Application::getInstance();

    ImGui::Begin("Databases", nullptr, ImGuiWindowFlags_NoScrollbar);

    // Check if we should show the connection dialog
    if (shouldShowConnectionDialog) {
        connectionDialog.showDialog();
        shouldShowConnectionDialog = false;
    }

    // Always render the dialog to handle multi-frame interactions
    if (connectionDialog.isDialogOpen()) {
        connectionDialog.showDialog();
    }

    // Check if dialog completed and get result
    if (const auto db = connectionDialog.getResult()) {
        auto [success, error] = db->connect();
        if (success) {
            // Only refresh tables immediately for SQLite, PostgreSQL will do it async when needed
            if (db->getType() == DatabaseType::SQLITE) {
                db->refreshTables();
            }
            std::cout << "Adding database to list." << std::endl;
            app.addDatabase(db);
        } else {
            std::cerr << "Failed to open database: " << error << std::endl;
        }
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 6.0f));
    auto &databases = app.getDatabases();

    if (databases.empty()) {
        // Show helpful message when no databases are connected
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextWrapped("No databases connected");
        ImGui::Spacing();
        ImGui::TextWrapped("Right-click here to add a new database connection");
        ImGui::PopStyleColor();

        // Show context menu for adding database when area is right-clicked
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("AddDatabasePopup");
        }

        if (ImGui::BeginPopup("AddDatabasePopup")) {
            if (ImGui::MenuItem("Add Database Connection")) {
                showConnectionDialog();
            }
            ImGui::EndPopup();
        }
    } else {
        for (size_t i = 0; i < databases.size(); i++) {
            renderDatabaseNode(i);
        }
    }

    ImGui::PopStyleVar();

    // Handle delete confirmation dialog
    if (shouldShowDeleteConfirmation) {
        ImGui::OpenPopup("Confirm Delete Database");
        shouldShowDeleteConfirmation = false;
    }

    if (ImGui::BeginPopupModal("Confirm Delete Database", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        auto &databases = app.getDatabases();
        if (databaseToDelete < databases.size()) {
            const auto &db = databases[databaseToDelete];
            ImGui::Text("Are you sure you want to remove this database connection?");
            ImGui::Text("Database: %s", db->getName().c_str());
            ImGui::Spacing();
            ImGui::Text("This will:");
            ImGui::BulletText("Remove the database from the current session");
            ImGui::BulletText("Delete the saved connection (if any)");
            ImGui::BulletText("Close any open tabs for this database");
            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Remove", ImVec2(100, 0))) {
                // Remove from saved connections by finding matching connection
                auto savedConnections = app.getAppState()->getSavedConnections();
                for (const auto &conn : savedConnections) {
                    bool matches = false;
                    if (db->getType() == DatabaseType::POSTGRESQL && conn.type == "postgresql") {
                        matches = (conn.name == db->getName());
                    } else if (db->getType() == DatabaseType::SQLITE && conn.type == "sqlite") {
                        matches = (conn.path == db->getPath());
                    }

                    if (matches) {
                        if (app.getAppState()->deleteConnection(conn.id)) {
                            std::cout << "deleted " << conn.id << std::endl;
                        }
                        break;
                    }
                }

                // Remove from application
                app.removeDatabase(databaseToDelete);

                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

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

    // Show loading indicator in database name if connecting
    std::string dbIcon;
    if (db->getType() == DatabaseType::SQLITE) {
        dbIcon = ICON_FK_DATABASE;
    } else if (db->getType() == DatabaseType::POSTGRESQL) {
        dbIcon = ICON_FK_POSTGRESQL;
    } else if (db->getType() == DatabaseType::MYSQL) {
        dbIcon = ICON_FK_MYSQL;
    } else if (db->getType() == DatabaseType::REDIS) {
        dbIcon = ICON_FA_DATABASE;
    } else {
        dbIcon = ICON_FK_DATABASE;
    }
    const bool showSpinner = db->isConnecting();

    // Draw tree node with placeholder space for icon
    const std::string dbLabel = std::format("   {}", db->getName()); // 3 spaces for icon
    const bool dbOpen = ImGui::TreeNodeEx(dbLabel.c_str(), dbFlags);

    // Draw colored icon over the placeholder space
    const auto dbIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    ImU32 iconColor;
    if (db->getType() == DatabaseType::SQLITE) {
        iconColor = ImGui::GetColorU32(ImVec4(0.3f, 0.7f, 1.0f, 1.0f)); // Light blue
    } else if (db->getType() == DatabaseType::POSTGRESQL) {
        iconColor = ImGui::GetColorU32(ImVec4(0.2f, 0.6f, 0.9f, 1.0f)); // Darker blue
    } else if (db->getType() == DatabaseType::MYSQL) {
        iconColor = ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)); // Orange
    } else if (db->getType() == DatabaseType::REDIS) {
        iconColor = ImGui::GetColorU32(ImVec4(1.0f, 0.2f, 0.2f, 1.0f)); // Red
    } else {
        iconColor = ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)); // Gray for unknown
    }

    ImGui::GetWindowDrawList()->AddText(dbIconPos, iconColor, dbIcon.c_str());

    // Show spinner next to database name if connecting
    if (showSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##db_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(static_cast<int>(databaseIndex));
        app.setSelectedTable(-1);
    }

    // Context menu for database
    ImGui::PushID(static_cast<int>(databaseIndex));
    handleDatabaseContextMenu(databaseIndex);
    ImGui::PopID();

    // Check for async connection completion (always check, even when collapsed)
    db->checkConnectionStatusAsync();

    if (dbOpen) {
        // Auto-connect when database node is expanded
        if (!db->isConnected() && !db->hasAttemptedConnection() && !db->isConnecting()) {
            std::cout << "Starting async connection to database: " << db->getName() << std::endl;
            db->startConnectionAsync();
        }

        // Check if database is connected
        if (db->isConnecting()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("  Connecting...");
            ImGui::SameLine();
            UIUtils::Spinner("##connecting_spinner", 6.0f, 2,
                             ImGui::GetColorU32(ImVec4(1.0f, 0.7f, 0.3f, 1.0f)));
            ImGui::PopStyleColor();
        } else if (!db->isConnected() && !db->hasAttemptedConnection()) {
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
            // Check for async loading completion for Postgres
            if (db->getType() == DatabaseType::POSTGRESQL) {
                auto *pgDb = dynamic_cast<PostgresDatabase *>(db.get());
                if (pgDb->isLoadingSchemas()) {
                    pgDb->checkSchemasStatusAsync();
                }
                if (db->isLoadingTables()) {
                    db->checkTablesStatusAsync();
                }
                if (db->isLoadingViews()) {
                    db->checkViewsStatusAsync();
                }
                if (db->isLoadingSequences()) {
                    db->checkSequencesStatusAsync();
                }
            }

            // Show hierarchical structure based on database type
            if (db->getType() == DatabaseType::SQLITE) {
                auto *sqliteDb = dynamic_cast<SQLiteDatabase *>(db.get());
                SQLiteHierarchy::renderSQLiteHierarchy(sqliteDb);
            } else if (db->getType() == DatabaseType::POSTGRESQL) {
                auto *pgDb = dynamic_cast<PostgresDatabase *>(db.get());
                PostgresHierarchy::renderPostgresHierarchy(pgDb);
            } else if (db->getType() == DatabaseType::MYSQL) {
                auto *mysqlDb = dynamic_cast<MySQLDatabase *>(db.get());
                // Check for async loading completion for MySQL
                if (db->isLoadingTables()) {
                    db->checkTablesStatusAsync();
                }
                if (db->isLoadingViews()) {
                    db->checkViewsStatusAsync();
                }
                MySQLHierarchy::renderMySQLHierarchy(mysqlDb);
            } else if (db->getType() == DatabaseType::REDIS) {
                // Check for async loading completion for Redis
                if (db->isLoadingTables()) {
                    db->checkTablesStatusAsync();
                }
                if (db->isLoadingTableData()) {
                    db->checkTableDataStatusAsync();
                }
                HierarchyHelpers::renderRedisHierarchy(db);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::handleDatabaseContextMenu(size_t databaseIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    if (ImGui::BeginPopupContextItem(nullptr)) {
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
            app.getTabManager()->createSQLEditorTab("", db->getConnectionString());
        }
        if (ImGui::MenuItem("Disconnect")) {
            db->disconnect();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Database")) {
            shouldShowDeleteConfirmation = true;
            databaseToDelete = databaseIndex;
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::handleTableContextMenu(const size_t databaseIndex, const size_t tableIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &table = db->getTables()[tableIndex];

    if (ImGui::BeginPopupContextItem(0)) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show table structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::renderTablesSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                     ImGuiTreeNodeFlags_FramePadding;

    // Show loading indicator next to Tables node if loading
    bool showTablesSpinner = (db->getType() == DatabaseType::POSTGRESQL && db->isLoadingTables());

    // Draw tree node with placeholder space for icon
    const std::string tablesLabel =
        std::format("   Tables ({})###tables",
                    db->getTables().size()); // 3 spaces for icon, ###tables for stable ID
    bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

    // Draw colored icon over the placeholder space
    const ImVec2 tablesSectionIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    ImGui::GetWindowDrawList()->AddText(
        tablesSectionIconPos,
        ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f)), // Green for Tables section
        ICON_FA_TABLE);

    // Show spinner next to Tables node if loading
    if (showTablesSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##tables_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    // Load tables when the tree node is opened and tables haven't been loaded yet
    if (tablesOpen && !db->areTablesLoaded() && !db->isLoadingTables()) {
        std::cout << "Tables node expanded and tables not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshTables();
    }

    if (tablesOpen) {
        if (db->getTables().empty()) {
            if (db->isLoadingTables()) {
                // Show loading indicator with spinner
                ImGui::Text("  Loading tables...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_tables_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            } else if (!db->areTablesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No tables found");
            }
        } else {
            for (size_t j = 0; j < db->getTables().size(); j++) {
                // renderTableNode(databaseIndex, j); // Moved to hierarchy modules
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

    // Show loading indicator next to Sequences node if loading
    const bool showSequencesSpinner =
        (db->getType() == DatabaseType::POSTGRESQL && db->isLoadingSequences());

    // Draw tree node with placeholder space for icon
    const std::string sequencesLabel =
        std::format("   Sequences ({})###sequences",
                    db->getSequences().size()); // 3 spaces for icon, ###sequences for stable ID
    const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

    // Draw colored icon over the placeholder space
    const auto sequencesSectionIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    ImGui::GetWindowDrawList()->AddText(
        sequencesSectionIconPos,
        ImGui::GetColorU32(ImVec4(0.8f, 0.3f, 0.8f, 1.0f)), // Purple for Sequences section
        ICON_FA_LIST_OL);

    // Show spinner next to Sequences node if loading
    if (showSequencesSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##sequences_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    // Load sequences when the tree node is opened and sequences haven't been loaded yet
    if (sequencesOpen && !db->areSequencesLoaded() && !db->isLoadingSequences()) {
        std::cout << "Sequences node expanded and sequences not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshSequences();
    }

    if (sequencesOpen) {
        if (db->getSequences().empty()) {
            if (db->isLoadingSequences()) {
                // Show loading indicator with spinner
                ImGui::Text("  Loading sequences...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_sequences_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            } else if (!db->areSequencesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No sequences found");
            }
        } else {
            for (size_t j = 0; j < db->getSequences().size(); j++) {
                // renderSequenceNode(databaseIndex, j); // Moved to hierarchy modules
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::handleViewContextMenu(const size_t databaseIndex, const size_t viewIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    const auto &view = db->getViews()[viewIndex];

    if (ImGui::BeginPopupContextItem(0)) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), view.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show view structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::handleSequenceContextMenu(const size_t databaseIndex,
                                                const size_t sequenceIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &sequence = db->getSequences()[sequenceIndex];

    if (ImGui::BeginPopupContextItem(0)) {
        if (ImGui::MenuItem("Show Details")) {
            // TODO: Show sequence details in a tab
        }
        ImGui::EndPopup();
    }
}
