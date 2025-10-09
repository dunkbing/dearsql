#include "ui/new/db_sidebar_new.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "ui/drop_column_dialog.hpp"
#include "ui/new/database_node.hpp"
#include "ui/tab_manager.hpp"
#include "ui/table_dialog.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <format>

// Static dialog instances
static TableDialog tableDialogNew;
static DropColumnDialog dropColumnDialogNew;

void DatabaseSidebarNew::showConnectionDialog() {
    shouldShowConnectionDialog = true;
}

void DatabaseSidebarNew::renderEmpty() {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
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
            Logger::info("Opening database connection dialog");
            showConnectionDialog();
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebarNew::render() {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGui::Begin("Databases", nullptr, ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 14.5f);

    // Check if we should show the connection dialog
    if (shouldShowConnectionDialog) {
        connectionDialog.showDialog();
        shouldShowConnectionDialog = false;
    }

    // Check if we should edit a connection
    if (databaseToEdit) {
        connectionDialog.editConnection(databaseToEdit);
        connectionDialog.showDialog(); // Open the dialog immediately
        databaseToEdit = nullptr;
    }

    // Always render the dialog to handle multi-frame interactions
    if (connectionDialog.isDialogOpen()) {
        connectionDialog.showDialog();
    }

    // Check if dialog completed and get result
    if (const auto db = connectionDialog.getResult()) {
        auto [success, error] = db->connect();
        if (success) {
            // Only refresh tables immediately for SQLite, Postgres will do it async when needed
            if (db->getType() == DatabaseType::SQLITE) {
                db->refreshTables();
            }
            Logger::info(std::format("Database connection established: {}", db->getName()));
            app.addDatabase(db);
        } else {
            Logger::error(
                std::format("Failed to connect to database '{}': {}", db->getName(), error));
        }
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 6.0f));
    const auto& databases = app.getDatabases();

    if (databases.empty()) {
        // Show helpful message when no databases are connected
        renderEmpty();
    } else {
        for (const auto& db : databases) {
            renderDatabaseNode(db);
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
        const auto db = databasePendingDeletion;
        if (db) {
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
                const auto savedConnections = app.getAppState()->getSavedConnections();

                if (app.getAppState()->deleteConnection(db->getSavedConnectionId())) {
                    Logger::info(std::format("Removed saved connection: {}", db->getName()));
                }
                Logger::info(std::format("Database removed: {}", db->getName()));
                app.removeDatabase(db);
                databasePendingDeletion.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                databasePendingDeletion.reset();
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::Text("No database selected for removal.");
        }
        ImGui::EndPopup();
    }

    // Render table dialog if open
    if (tableDialogNew.isDialogOpen()) {
        tableDialogNew.renderDialog();
    }

    // Handle table dialog completion
    if (tableDialogNew.hasResult()) {
        tableDialogNew.clearResult();
    }

    // Render drop column dialog if open
    if (dropColumnDialogNew.isDialogOpen()) {
        dropColumnDialogNew.renderDialog();
    }

    // Handle drop column dialog completion
    if (dropColumnDialogNew.hasResult()) {
        dropColumnDialogNew.clearResult();
    }

    // Handle create database dialog
    if (shouldShowCreateDatabaseDialog) {
        ImGui::OpenPopup("Create Database");
        shouldShowCreateDatabaseDialog = false;
    }

    if (ImGui::BeginPopupModal("Create Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto db = createDatabaseTarget;
        static char dbName[256] = "";
        static char dbComment[512] = "";
        static std::string errorMessage;

        if (!db) {
            ImGui::Text("No connection selected.");
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                createDatabaseTarget.reset();
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::Text("Create new database on:");
            ImGui::Text("Connection: %s", db->getName().c_str());
            ImGui::Text("Type: %s",
                        db->getType() == DatabaseType::POSTGRESQL ? "PostgreSQL" : "MySQL");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Database Name:");
            ImGui::SetNextItemWidth(300);
            ImGui::InputText("##db_name", dbName, sizeof(dbName));

            ImGui::Spacing();

            if (db->getType() == DatabaseType::MYSQL) {
                ImGui::Text("Comment (optional):");
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##db_comment", dbComment, sizeof(dbComment));
                ImGui::Spacing();
            }

            if (!errorMessage.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
                ImGui::TextWrapped("Error: %s", errorMessage.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_Button, colors.blue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.sky);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.sapphire);

            if (ImGui::Button("Create Database", ImVec2(120, 0))) {
                if (strlen(dbName) == 0) {
                    errorMessage = "Database name cannot be empty";
                } else {
                    std::string sql;
                    if (db->getType() == DatabaseType::POSTGRESQL) {
                        sql = std::format(R"(CREATE DATABASE "{}")", dbName);
                    } else if (db->getType() == DatabaseType::MYSQL) {
                        sql = std::format("CREATE DATABASE `{}`", dbName);
                        if (strlen(dbComment) > 0) {
                            sql += std::format(" COMMENT '{}'", dbComment);
                        }
                    }

                    const std::string result = db->executeQuery(sql);
                    if (result.find("Error") != std::string::npos) {
                        errorMessage = result;
                    } else {
                        Logger::info(std::format("Database '{}' created successfully", dbName));
                        memset(dbName, 0, sizeof(dbName));
                        memset(dbComment, 0, sizeof(dbComment));
                        errorMessage.clear();
                        ImGui::CloseCurrentPopup();

                        if (db->getType() == DatabaseType::POSTGRESQL) {
                            if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                                pgDb->refreshDatabaseNames();
                            }
                        } else if (db->getType() == DatabaseType::MYSQL) {
                            if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
                                mysqlDb->refreshDatabaseNames();
                            }
                        }
                        createDatabaseTarget.reset();
                    }
                }
            }

            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, colors.overlay0);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.overlay1);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay2);

            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                memset(dbName, 0, sizeof(dbName));
                memset(dbComment, 0, sizeof(dbComment));
                errorMessage.clear();
                createDatabaseTarget.reset();
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopStyleColor(3);
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void DatabaseSidebarNew::renderDatabaseNode(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags dbFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_FramePadding;
    if (const auto selected = app.getSelectedDatabase(); selected && selected == db) {
        dbFlags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool showSpinner = db->isConnecting();
    std::string icon;
    switch (db->getType()) {
    case DatabaseType::SQLITE:
        icon = ICON_FK_DATABASE;
        break;
    case DatabaseType::POSTGRESQL:
        icon = ICON_FK_POSTGRESQL;
        break;
    case DatabaseType::MYSQL:
        icon = ICON_FK_MYSQL;
        break;
    case DatabaseType::REDIS:
        icon = ICON_FK_DATABASE;
        break;
    default:
        icon = ICON_FK_DATABASE;
        break;
    }

    const std::string dbLabel =
        std::format("   {}###db_{:p}", db->getName(), static_cast<const void*>(db.get()));
    const bool dbOpen = ImGui::TreeNodeEx(dbLabel.c_str(), dbFlags);

    const auto dbIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    ImU32 iconColor = ImGui::GetColorU32(colors.overlay1);
    switch (db->getType()) {
    case DatabaseType::SQLITE:
        iconColor = ImGui::GetColorU32(colors.sky);
        break;
    case DatabaseType::POSTGRESQL:
        iconColor = ImGui::GetColorU32(colors.blue);
        break;
    case DatabaseType::MYSQL:
        iconColor = ImGui::GetColorU32(colors.peach);
        break;
    case DatabaseType::REDIS:
        iconColor = ImGui::GetColorU32(colors.red);
        break;
    default:
        break;
    }
    ImGui::GetWindowDrawList()->AddText(dbIconPos, iconColor, icon.c_str());

    if (showSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##db_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(db);
    }

    ImGui::PushID(db.get());
    handleDatabaseContextMenu(db);
    ImGui::PopID();

    db->checkConnectionStatusAsync();

    if (dbOpen) {
        if (!db->isConnected() && !db->hasAttemptedConnection() && !db->isConnecting()) {
            Logger::info(std::format("Starting connection to database: {}", db->getName()));
            db->startConnectionAsync();
        }

        if (db->isConnecting()) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Connecting...");
            ImGui::SameLine();
            UIUtils::Spinner("##connecting_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (!db->isConnected() && !db->hasAttemptedConnection()) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Click to connect");
            ImGui::PopStyleColor();
        } else if (db->hasAttemptedConnection() && !db->isConnected() &&
                   !db->getLastConnectionError().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
            ImGui::TextWrapped("  Connection failed: %s", db->getLastConnectionError().c_str());
            ImGui::PopStyleColor();
        } else if (db->isConnected()) {
            // Use the new refactored hierarchy rendering
            NewHierarchy::renderRootDatabaseNode(db);
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebarNew::handleDatabaseContextMenu(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        return;
    }

    auto& app = Application::getInstance();

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
        if (ImGui::MenuItem("Refresh")) {
            db->disconnect();
            db->setAttemptedConnection(false);
            db->setTablesLoaded(false);
            db->setViewsLoaded(false);
            db->setSequencesLoaded(false);
            db->setLastConnectionError("");
            db->startConnectionAsync();
        }
        if (ImGui::MenuItem("New SQL Editor")) {
            app.getTabManager()->createSQLEditorTab("", db);
        }
        if (ImGui::MenuItem("Show Diagram")) {
            app.getTabManager()->createDiagramTab(db);
        }
        ImGui::Separator();

        if (db->getType() == DatabaseType::POSTGRESQL || db->getType() == DatabaseType::MYSQL) {
            if (ImGui::MenuItem("Create new database")) {
                shouldShowCreateDatabaseDialog = true;
                createDatabaseTarget = db;
            }
        }

        if (ImGui::MenuItem("Edit connection")) {
            databaseToEdit = db;
        }

        if (ImGui::MenuItem("Disconnect")) {
            db->disconnect();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Database")) {
            shouldShowDeleteConfirmation = true;
            databasePendingDeletion = db;
        }
        ImGui::EndPopup();
    }
}
