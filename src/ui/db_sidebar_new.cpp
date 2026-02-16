#include "ui/db_sidebar_new.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/query_executor.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "ui/confirm_dialog.hpp"
#include "ui/database_node.hpp"
#include "ui/input_dialog.hpp"
#include "ui/query_history.hpp"
#include "ui/table_dialog.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <chrono>
#include <format>
#include <memory>

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
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
        if (ImGui::MenuItem("Add Database Connection")) {
            Logger::info("Opening database connection dialog");
            showConnectionDialog();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseSidebarNew::renderStructure() {
    auto& app = Application::getInstance();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
    const auto& databases = app.getDatabases();

    if (databases.empty()) {
        renderEmpty();
    } else {
        for (const auto& db : databases) {
            renderDatabaseNode(db);
        }
    }

    ImGui::PopStyleVar(2);
}

void DatabaseSidebarNew::renderHistory() {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();
    auto& history = QueryHistory::instance();

    const auto& entries = history.getEntries();
    if (entries.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextWrapped("No queries executed yet");
        ImGui::PopStyleColor();
        return;
    }

    // Calculate relative time
    auto formatRelativeTime = [](const std::chrono::system_clock::time_point& tp) -> std::string {
        const auto now = std::chrono::system_clock::now();
        const auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();

        if (diff < 60) {
            return "just now";
        }
        if (diff < 3600) {
            int mins = static_cast<int>(diff / 60);
            return std::format("{}m ago", mins);
        }
        if (diff < 86400) {
            int hours = static_cast<int>(diff / 3600);
            return std::format("{}h ago", hours);
        }
        int days = static_cast<int>(diff / 86400);
        return std::format("{}d ago", days);
    };

    // Get query type label and color
    auto getQueryTypeInfo = [&colors](QueryType type) -> std::pair<std::string, ImVec4> {
        switch (type) {
        case QueryType::SELECT:
            return {"SELECT", colors.blue};
        case QueryType::INSERT:
            return {"INSERT", colors.green};
        case QueryType::UPDATE:
            return {"UPDATE", colors.peach};
        case QueryType::DELETE:
            return {"DELETE", colors.red};
        case QueryType::CREATE:
            return {"CREATE", colors.mauve};
        case QueryType::ALTER:
            return {"ALTER", colors.yellow};
        case QueryType::DROP:
            return {"DROP", colors.maroon};
        default:
            return {"OTHER", colors.overlay1};
        }
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto [typeLabel, typeColor] = getQueryTypeInfo(entry.type);

        ImGui::PushID(static_cast<int>(i));

        // Query type badge
        ImGui::PushStyleColor(ImGuiCol_Button, typeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, typeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, typeColor);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
        ImGui::SmallButton(typeLabel.c_str());
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        // Truncated query text
        const float availWidth = ImGui::GetContentRegionAvail().x - 30.0f;
        std::string displayQuery = entry.query;
        if (displayQuery.length() > 30) {
            displayQuery = displayQuery.substr(0, 27) + "...";
        }

        // Make the query text clickable (selectable)
        ImGui::PushStyleColor(ImGuiCol_Text, colors.text);
        if (ImGui::Selectable(displayQuery.c_str(), false, 0, ImVec2(availWidth, 0))) {
            // TODO: Could copy to clipboard or open in SQL editor
        }
        ImGui::PopStyleColor();

        // Tooltip with full query on hover
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(400.0f);
            ImGui::TextUnformatted(entry.query.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem("history_entry_menu")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
            if (ImGui::MenuItem("Copy to clipboard")) {
                ImGui::SetClipboardText(entry.query.c_str());
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        // Metadata line (time, rows, duration)
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        std::string metaInfo = formatRelativeTime(entry.timestamp);
        if (entry.rowCount > 0) {
            metaInfo += std::format(" {} rows", entry.rowCount);
        }
        if (entry.durationMs > 0) {
            metaInfo += std::format(" {}ms", entry.durationMs);
        }
        ImGui::Text("%s %s", ICON_FA_CLOCK, metaInfo.c_str());
        ImGui::PopStyleColor();

        ImGui::PopID();
    }

    ImGui::PopStyleVar(2);
}

void DatabaseSidebarNew::render() {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    // Square popup corners
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
    ImGui::Begin("Databases", nullptr, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    ImGui::PushStyleColor(ImGuiCol_Header,
                          ImVec4(colors.surface1.x, colors.surface1.y, colors.surface1.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImVec4(colors.surface1.x, colors.surface1.y, colors.surface1.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.3f));

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8.0f);

    if (shouldShowConnectionDialog) {
        connectionDialog.showDialog();
        shouldShowConnectionDialog = false;
    }

    if (databaseToEdit) {
        connectionDialog.editConnection(databaseToEdit);
        connectionDialog.showDialog();
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

    ImGui::PushStyleColor(ImGuiCol_Separator,
                          ImVec4(colors.overlay0.x, colors.overlay0.y, colors.overlay0.z, 0.2f));
    ImGui::Separator();
    ImGui::PopStyleColor();

    // Calculate available height for the sections
    const float availableHeight = ImGui::GetContentRegionAvail().y;
    constexpr float historyHeight = 300.0f;

    // Structure section (top) - scrollbar visible only on hover
    const float structureSectionHeight =
        availableHeight - historyHeight - ImGui::GetStyle().ItemSpacing.y;
    const bool structureHovered = ImGui::IsMouseHoveringRect(
        ImGui::GetCursorScreenPos(),
        ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
               ImGui::GetCursorScreenPos().y + structureSectionHeight));
    ImGuiWindowFlags structureFlags = structureHovered ? 0 : ImGuiWindowFlags_NoScrollbar;
    ImGui::BeginChild("StructureSection", ImVec2(0, structureSectionHeight), false, structureFlags);
    renderStructure();
    ImGui::EndChild();

    ImGui::PushStyleColor(ImGuiCol_Separator,
                          ImVec4(colors.overlay0.x, colors.overlay0.y, colors.overlay0.z, 0.3f));
    ImGui::Separator();
    ImGui::PopStyleColor();

    auto& history = QueryHistory::instance();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::TextUnformatted("HISTORY");
    ImGui::PopStyleColor();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(colors.surface1.x, colors.surface1.y, colors.surface1.z, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
    if (ImGui::Button(ICON_FA_TRASH_CAN "##clear_history")) {
        history.clear();
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear history");
    }
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    // History list section (scrollable) - scrollbar visible only on hover
    const ImVec2 historyCursorPos = ImGui::GetCursorScreenPos();
    const bool historyHovered = ImGui::IsMouseHoveringRect(
        historyCursorPos, ImVec2(historyCursorPos.x + ImGui::GetContentRegionAvail().x,
                                 historyCursorPos.y + ImGui::GetContentRegionAvail().y));
    ImGuiWindowFlags historyFlags = historyHovered ? 0 : ImGuiWindowFlags_NoScrollbar;
    ImGui::BeginChild("HistorySection", ImVec2(0, 0), false, historyFlags);
    renderHistory();
    ImGui::EndChild();

    // Handle delete confirmation dialog
    if (shouldShowDeleteConfirmation && databasePendingDeletion) {
        const auto db = databasePendingDeletion;
        auto const connectionInfo = db->getConnectionInfo();

        ConfirmDialog::instance().show(
            "Remove Database",
            std::format("Are you sure you want to remove '{}'?", connectionInfo.name),
            {"Remove the database from the current session", "Delete the saved connection (if any)",
             "Close any open tabs for this database"},
            "Remove",
            [this, db, &app, connectionInfo]() {
                if (app.getAppState()->deleteConnection(db->getConnectionId())) {
                    Logger::info(std::format("Removed saved connection: {}", connectionInfo.name));
                }
                Logger::info(std::format("Database removed: {}", connectionInfo.name));
                hierarchyCache.erase(db.get());
                app.removeDatabase(db);
                databasePendingDeletion.reset();
            },
            [this]() { databasePendingDeletion.reset(); });

        shouldShowDeleteConfirmation = false;
    }

    // dialogs
    if (TableDialog::instance().isOpen()) {
        TableDialog::instance().render();
    }

    if (InputDialog::instance().isOpen()) {
        InputDialog::instance().render();
    }

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
                        const auto queryResults = executor->executeQuery(sql);
                        const auto& queryResult =
                            queryResults.empty() ? QueryResult{} : queryResults.back();
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

    ImGui::PopStyleColor(3);
    ImGui::End();

    ImGui::PopStyleVar(); // PopupRounding
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
    case DatabaseType::MONGODB:
        icon = ICON_FK_DATABASE;
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
    case DatabaseType::MONGODB:
        iconColor = ImGui::GetColorU32(colors.green);
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

    // Check refresh workflow status
    if (type == DatabaseType::POSTGRESQL) {
        if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
            pgDb->checkRefreshWorkflowAsync();
        }
    } else if (type == DatabaseType::MYSQL) {
        if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
            mysqlDb->checkRefreshWorkflowAsync();
        }
    } else if (type == DatabaseType::MONGODB) {
        if (auto* mongoDb = dynamic_cast<MongoDBDatabase*>(db.get())) {
            mongoDb->checkRefreshWorkflowAsync();
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
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
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
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}
