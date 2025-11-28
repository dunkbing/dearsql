#include "ui/new/db_sidebar_new.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/query_executor.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "ui/confirm_dialog.hpp"
#include "ui/drop_column_dialog.hpp"
#include "ui/new/database_node.hpp"
#include "ui/rename_dialog.hpp"
#include "ui/table_dialog.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <format>
#include <memory>

// Static dialog instances (non-singleton dialogs)
static TableDialog tableDialogNew;
static DropColumnDialog dropColumnDialogNew;

// Function to access the dialogs from database_node rendering
namespace NewHierarchy {
    TableDialog& getTableDialog() {
        return tableDialogNew;
    }

    DropColumnDialog& getDropColumnDialog() {
        return dropColumnDialogNew;
    }
} // namespace NewHierarchy

DatabaseHierarchy* DatabaseSidebarNew::getHierarchy(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        return nullptr;
    }

    auto it = hierarchyCache.find(db.get());
    if (it != hierarchyCache.end()) {
        return it->second.get();
    }

    // Create new hierarchy if not found (shouldn't happen if syncHierarchyCache is called)
    auto [inserted, success] =
        hierarchyCache.emplace(db.get(), std::make_unique<DatabaseHierarchy>(db));
    return inserted->second.get();
}

void DatabaseSidebarNew::showConnectionDialog() {
    shouldShowConnectionDialog = true;
}

void DatabaseSidebarNew::renderEmpty() {
    const auto& app = Application::getInstance();
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
            Logger::info(
                std::format("Database connection established: {}", db->getConnectionInfo().name));
            app.addDatabase(db);
        } else {
            Logger::error(std::format("Failed to connect to database '{}': {}",
                                      db->getConnectionInfo().name, error));
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
            auto const connectionInfo = db->getConnectionInfo();
            ImGui::Text("Are you sure you want to remove this database connection?");
            ImGui::Text("Database: %s", connectionInfo.name.c_str());
            ImGui::Spacing();
            ImGui::Text("This will:");
            ImGui::BulletText("Remove the database from the current session");
            ImGui::BulletText("Delete the saved connection (if any)");
            ImGui::BulletText("Close any open tabs for this database");
            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Remove", ImVec2(100, 0))) {
                if (app.getAppState()->deleteConnection(db->getConnectionId())) {
                    Logger::info(std::format("Removed saved connection: {}", connectionInfo.name));
                }
                Logger::info(std::format("Database removed: {}", connectionInfo.name));
                // Remove from hierarchy cache before removing database
                hierarchyCache.erase(db.get());
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

    // Render rename dialog if open
    if (RenameDialog::instance().isOpen()) {
        RenameDialog::instance().render();
    }

    // Render confirm dialog if open
    if (ConfirmDialog::instance().isOpen()) {
        ConfirmDialog::instance().render();
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
            auto const connectionInfo = db->getConnectionInfo();
            const auto dbType = connectionInfo.type;
            ImGui::Text("Create new database on:");
            ImGui::Text("Connection: %s", connectionInfo.name.c_str());
            ImGui::Text("Type: %s", dbType == DatabaseType::POSTGRESQL ? "PostgreSQL" : "MySQL");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Database Name:");
            ImGui::SetNextItemWidth(300);
            ImGui::InputText("##db_name", dbName, sizeof(dbName));

            ImGui::Spacing();

            if (dbType == DatabaseType::MYSQL) {
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
                    if (dbType == DatabaseType::POSTGRESQL) {
                        sql = std::format(R"(CREATE DATABASE "{}")", dbName);
                    } else if (dbType == DatabaseType::MYSQL) {
                        sql = std::format("CREATE DATABASE `{}`", dbName);
                        if (strlen(dbComment) > 0) {
                            sql += std::format(" COMMENT '{}'", dbComment);
                        }
                    }

                    auto executor = std::dynamic_pointer_cast<IQueryExecutor>(db);
                    if (!executor) {
                        errorMessage = "Database does not support query execution";
                    } else {
                        const auto queryResult = executor->executeQueryWithResult(sql);
                        if (!queryResult.success) {
                            errorMessage = queryResult.errorMessage;
                        } else {
                            Logger::info(std::format("Database '{}' created successfully", dbName));
                            memset(dbName, 0, sizeof(dbName));
                            memset(dbComment, 0, sizeof(dbComment));
                            errorMessage.clear();
                            ImGui::CloseCurrentPopup();

                            if (dbType == DatabaseType::POSTGRESQL) {
                                if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                                    pgDb->refreshDatabaseNames();
                                }
                            } else if (dbType == DatabaseType::MYSQL) {
                                if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
                                    mysqlDb->refreshDatabaseNames();
                                }
                            }
                            createDatabaseTarget.reset();
                        }
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

    auto const connectionInfo = db->getConnectionInfo();
    auto const type = connectionInfo.type;
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
    switch (type) {
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
        std::format("   {}###db_{:p}", connectionInfo.name, static_cast<const void*>(db.get()));
    const bool dbOpen = ImGui::TreeNodeEx(dbLabel.c_str(), dbFlags);

    const auto dbIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    ImU32 iconColor = ImGui::GetColorU32(colors.overlay1);
    switch (type) {
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

    // Check refresh workflow status for PostgreSQL
    if (type == DatabaseType::POSTGRESQL) {
        if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
            pgDb->checkRefreshWorkflowAsync();
        }
    }

    // Check refresh workflow status for MySQL
    else if (type == DatabaseType::MYSQL) {
        if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
            mysqlDb->checkRefreshWorkflowAsync();
        }
    }

    if (dbOpen) {
        if (!db->isConnected() && !db->hasAttemptedConnection() && !db->isConnecting()) {
            Logger::info(std::format("Starting connection to database: {}", connectionInfo.name));
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
            // Use cached hierarchy for rendering (avoids creating new objects every frame)
            if (auto* hierarchy = getHierarchy(db)) {
                hierarchy->renderRootNode();
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebarNew::handleDatabaseContextMenu(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        return;
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        // SQLite-specific menu items (only when connected)
        if (db->isConnected() && db->getConnectionInfo().type == DatabaseType::SQLITE) {
            auto* sqliteDb = dynamic_cast<SQLiteDatabase*>(db.get());
            if (sqliteDb) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    Application::getInstance().getTabManager()->createSQLEditorTab("", sqliteDb);
                }
                if (ImGui::MenuItem("Show Diagram")) {
                    Application::getInstance().getTabManager()->createDiagramTab(sqliteDb);
                }
                ImGui::Separator();
            }
        }

        if (ImGui::MenuItem("Edit connection")) {
            databaseToEdit = db;
        }

        if (db->isConnected()) {
            if (ImGui::MenuItem("Disconnect")) {
                db->disconnect();
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Remove Database")) {
            shouldShowDeleteConfirmation = true;
            databasePendingDeletion = db;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Refresh")) {
            Logger::info(std::format("Refreshing connection for database: {}",
                                     db->getConnectionInfo().name));
            db->refreshConnection();
        }
        ImGui::EndPopup();
    }
}
