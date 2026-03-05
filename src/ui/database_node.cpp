#include "ui/database_node.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "platform/alert.hpp"
#include "ui/input_dialog.hpp"
#include "ui/tab_manager.hpp"
#include "ui/table_dialog.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <format>
#include <ranges>

namespace {
    constexpr const char* CREATE_TABLE_LABEL = "Create Table";
    constexpr const char* REFRESH_LABEL = "Refresh";
    constexpr const char* DELETE_LABEL = "Delete";
    constexpr const char* RENAME_LABEL = "Rename";
    constexpr const char* VIEW_DATA_LABEL = "View Data";
    constexpr const char* EDIT_TABLE_LABEL = "Edit Table";
    constexpr const char* SHOW_STRUCTURE_LABEL = "Show Structure";
    constexpr const char* NEW_SQL_EDITOR_LABEL = "New SQL Editor";
    constexpr const char* SHOW_DIAGRAM_LABEL = "Show Diagram";
} // namespace

DatabaseHierarchy::DatabaseHierarchy(std::shared_ptr<DatabaseInterface> dbInterface)
    : db(std::move(dbInterface)) {}

bool DatabaseHierarchy::renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                                               const std::string& icon, const ImU32 iconColor,
                                               const ImGuiTreeNodeFlags flags) {
    const std::string fullLabel = std::format("   {}###{}", label, nodeId);

    ImGui::PushID(nodeId.c_str());
    const ImGuiID id = ImGui::GetID("hover");
    ImGui::PopID();

    const float dt = ImGui::GetIO().DeltaTime;

    // check if this item will be hovered (predict based on cursor position)
    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    const float itemHeight = ImGui::GetFrameHeight();
    const float itemWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 itemMin = cursorPos;
    const ImVec2 itemMax = ImVec2(cursorPos.x + itemWidth, cursorPos.y + itemHeight);
    const bool anyPopupOpen =
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool willBeHovered = !anyPopupOpen && ImGui::IsMouseHoveringRect(itemMin, itemMax);

    if (willBeHovered) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImVec4 hoverColor = colors.surface2;
        hoverColor.w = 0.8f;
        ImGui::GetWindowDrawList()->AddRectFilled(itemMin, itemMax,
                                                  ImGui::ColorConvertFloat4ToU32(hoverColor), 0);
    }

    const bool isOpen = ImGui::TreeNodeEx(fullLabel.c_str(), flags);

    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, iconColor, icon.c_str());

    return isOpen;
}

void DatabaseHierarchy::renderRootNode() {
    if (!db) {
        return;
    }

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const auto dbType = db->getConnectionInfo().type;

    if (dbType == DatabaseType::SQLITE) {
        renderSQLiteNode();
    } else if (dbType == DatabaseType::POSTGRESQL) {
        auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get());
        if (!pgDb) {
            return;
        }

        if (!pgDb->areDatabasesLoaded() && !pgDb->isLoadingDatabases()) {
            pgDb->refreshDatabaseNames();
        }

        if (pgDb->isLoadingDatabases()) {
            pgDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Loading databases...");
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (pgDb->areDatabasesLoaded()) {
            const auto& databases = pgDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr) {
                    renderPostgresDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::MYSQL || dbType == DatabaseType::MARIADB) {
        auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get());
        if (!mysqlDb) {
            return;
        }

        // Multi-database mode
        if (!mysqlDb->areDatabasesLoaded() && !mysqlDb->isLoadingDatabases()) {
            mysqlDb->refreshDatabaseNames();
        }

        if (mysqlDb->isLoadingDatabases()) {
            mysqlDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Loading databases...");
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (mysqlDb->areDatabasesLoaded()) {
            const auto& databases = mysqlDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr) {
                    renderMySQLDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::MONGODB) {
        auto* mongoDb = dynamic_cast<MongoDBDatabase*>(db.get());
        if (!mongoDb) {
            return;
        }

        if (!mongoDb->areDatabasesLoaded() && !mongoDb->isLoadingDatabases()) {
            mongoDb->refreshDatabaseNames();
        }

        if (mongoDb->isLoadingDatabases()) {
            mongoDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Loading databases...");
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (mongoDb->areDatabasesLoaded()) {
            const auto& databases = mongoDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr) {
                    renderMongoDBDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::MSSQL) {
        auto* mssqlDb = dynamic_cast<MSSQLDatabase*>(db.get());
        if (!mssqlDb) {
            return;
        }

        if (!mssqlDb->areDatabasesLoaded() && !mssqlDb->isLoadingDatabases()) {
            mssqlDb->refreshDatabaseNames();
        }

        if (mssqlDb->isLoadingDatabases()) {
            mssqlDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Loading databases...");
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (mssqlDb->areDatabasesLoaded()) {
            const auto& databases = mssqlDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr) {
                    renderMSSQLDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::REDIS) {
        const auto redisDb = std::dynamic_pointer_cast<RedisDatabase>(db);
        if (!redisDb) {
            return;
        }

        // Show connection status
        if (!redisDb->isConnected()) {
            if (redisDb->isConnecting()) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                                   ICON_FA_SPINNER " Connecting...");
            } else if (redisDb->hasAttemptedConnection()) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                   ICON_FA_CIRCLE_EXCLAMATION " Connection failed");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", redisDb->getLastConnectionError().c_str());
                }
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                   ICON_FA_DATABASE " Not connected");
            }
            return;
        }

        // Commands node — opens a Redis CLI editor tab
        {
            constexpr ImGuiTreeNodeFlags cmdFlags = ImGuiTreeNodeFlags_Leaf |
                                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_FramePadding;
            const std::string cmdId =
                std::format("redis_commands_{:p}", static_cast<const void*>(redisDb.get()));
            const std::string cmdLabel = std::format("   Commands###{}", cmdId);
            ImGui::TreeNodeEx(cmdLabel.c_str(), cmdFlags);

            const auto iconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.mauve),
                                                ICON_FA_TERMINAL);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                Application::getInstance().getTabManager()->createRedisCommandEditorTab(
                    redisDb.get());
            }

            if (ImGui::BeginPopupContextItem(cmdId.c_str())) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem("Open Command Editor")) {
                    Application::getInstance().getTabManager()->createRedisCommandEditorTab(
                        redisDb.get());
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
        }

        // Pub/Sub node
        {
            constexpr ImGuiTreeNodeFlags pubsubFlags = ImGuiTreeNodeFlags_Leaf |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                       ImGuiTreeNodeFlags_FramePadding;
            const std::string pubsubId =
                std::format("redis_pubsub_{:p}", static_cast<const void*>(redisDb.get()));
            const std::string pubsubLabel = std::format("   Pub/Sub###{}", pubsubId);
            ImGui::TreeNodeEx(pubsubLabel.c_str(), pubsubFlags);

            const auto pubsubIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(pubsubIconPos, ImGui::GetColorU32(colors.green),
                                                ICON_FA_TOWER_BROADCAST);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                Application::getInstance().getTabManager()->createRedisPubSubTab(redisDb.get());
            }

            if (ImGui::BeginPopupContextItem(pubsubId.c_str())) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem("Open Pub/Sub")) {
                    Application::getInstance().getTabManager()->createRedisPubSubTab(redisDb.get());
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
        }

        // Load keys if not loaded yet
        if (!redisDb->keysLoaded && !redisDb->loadingKeys.load()) {
            redisDb->startKeysLoadAsync();
        }

        // Check async status
        if (redisDb->loadingKeys.load()) {
            redisDb->checkKeysStatusAsync();
        }

        // Show loading indicator if loading
        if (redisDb->loadingKeys.load()) {
            ImGui::SameLine();
            ImGui::Text("Loading keys...");
            return;
        }

        // Show key groups directly (no nested Keys section)
        const auto& keyGroups = redisDb->getKeyGroups();
        if (keyGroups.empty()) {
            if (!redisDb->keysLoaded) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No keys found");
            }
        } else {
            for (const auto& keyGroup : keyGroups) {
                constexpr ImGuiTreeNodeFlags keyGroupFlags = ImGuiTreeNodeFlags_Leaf |
                                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                             ImGuiTreeNodeFlags_FramePadding;

                const std::string displayName = (keyGroup.name == "*") ? "Browse" : keyGroup.name;
                const std::string keyGroupId = std::format("redis_key_{}_{:p}", displayName,
                                                           static_cast<const void*>(&keyGroup));

                renderTreeNodeWithIcon(displayName, keyGroupId, ICON_FA_KEY,
                                       ImGui::GetColorU32(colors.yellow), keyGroupFlags);

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    Application::getInstance().getTabManager()->createRedisKeyViewerTab(
                        redisDb.get(), keyGroup.name);
                }

                // Context menu
                if (ImGui::BeginPopupContextItem(keyGroupId.c_str())) {
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                        ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                    if (ImGui::MenuItem("View Keys")) {
                        Application::getInstance().getTabManager()->createRedisKeyViewerTab(
                            redisDb.get(), keyGroup.name);
                    }
                    if (ImGui::MenuItem("Refresh Keys")) {
                        redisDb->startKeysLoadAsync(true);
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndPopup();
                }
            }
        }
    }
}

void DatabaseHierarchy::renderSQLiteNode() {
    auto* sqliteDb = dynamic_cast<SQLiteDatabase*>(db.get());
    if (!sqliteDb) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();

    // Render Tables section
    {
        const std::string tablesNodeId =
            std::format("sqlite_tables_{:p}", static_cast<void*>(sqliteDb));
        const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                       ImGui::GetColorU32(colors.green));

        // Context menu for Tables node
        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                TableDialog::instance().showCreate(sqliteDb);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                sqliteDb->startTablesLoadAsync();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (tablesOpen) {
            if (!sqliteDb->areTablesLoaded() && !sqliteDb->loadingTables) {
                sqliteDb->startTablesLoadAsync();
            }

            if (sqliteDb->loadingTables) {
                sqliteDb->checkTablesStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::Text("  Loading tables...");
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (sqliteDb->tablesLoaded) {
                auto& tables = const_cast<std::vector<Table>&>(
                    const_cast<const SQLiteDatabase*>(sqliteDb)->getTables());
                if (tables.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No tables");
                    ImGui::PopStyleColor();
                } else {
                    for (auto& table : tables) {
                        renderSQLiteTableNode(table, sqliteDb);
                    }
                }
            }
            ImGui::TreePop();
        }
    }

    // Render Views section
    {
        const std::string viewsNodeId =
            std::format("sqlite_views_{:p}", static_cast<void*>(sqliteDb));
        const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                      ImGui::GetColorU32(colors.teal));

        // Context menu for Views node
        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                sqliteDb->startViewsLoadAsync();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (viewsOpen) {
            if (!sqliteDb->viewsLoaded && !sqliteDb->loadingViews.load()) {
                sqliteDb->startViewsLoadAsync();
            }

            if (sqliteDb->loadingViews) {
                sqliteDb->checkViewsStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::Text("  Loading views...");
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else {
                auto& views = const_cast<std::vector<Table>&>(
                    const_cast<const SQLiteDatabase*>(sqliteDb)->getViews());
                if (views.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No views");
                    ImGui::PopStyleColor();
                } else {
                    for (auto& view : views) {
                        renderSQLiteViewNode(view, sqliteDb);
                    }
                }
            }
            ImGui::TreePop();
        }
    }
}

void DatabaseHierarchy::renderPostgresDatabaseNode(PostgresDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.blue));

    // Handle expand/collapse
    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            // For PostgreSQL, we need to use a schema node (which implements IDatabaseNode)
            if (dbData->schemasLoaded && !dbData->schemas.empty()) {
                auto* schemaNode = dbData->schemas[0].get();
                app.getTabManager()->createSQLEditorTab("", schemaNode);
            }
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startSchemasLoadAsync(true, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = dbData->name;
            InputDialog::instance().showWithValidation(
                "Rename Database", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [this, oldName](const std::string& newName) {
                    auto [success, error] = db->renameDatabase(oldName, newName);
                    if (success) {
                        if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                            pgDb->refreshDatabaseNames();
                        }
                    } else {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [this, dbName]() {
                      auto [success, error] = db->dropDatabase(dbName);
                      if (success) {
                          Logger::info(std::format("Database '{}' deleted successfully", dbName));
                          if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                              pgDb->refreshDatabaseNames();
                          }
                      } else {
                          Logger::error(std::format("Failed to delete database: {}", error));
                          Alert::show("Error", std::format("Failed to delete database: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // PostgreSQL: render schemas
        if (!dbData->schemasLoaded && !dbData->schemasLoader.isRunning()) {
            dbData->startSchemasLoadAsync();
        }

        if (dbData->schemasLoader.isRunning()) {
            dbData->checkSchemasStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Loading schemas...");
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_schemas", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (dbData->schemasLoaded) {
            // Render each schema
            for (auto& schema : dbData->schemas) {
                renderPostgresSchemaNode(dbData, schema.get());
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderPostgresSchemaNode(const PostgresDatabaseNode* dbData,
                                                 PostgresSchemaNode* schemaData) {
    if (!dbData || !schemaData) {
        return;
    }

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId =
        std::format("schema_{}_{:p}", schemaData->name, static_cast<void*>(schemaData));
    const bool isOpen = renderTreeNodeWithIcon(schemaData->name, nodeId, ICON_FK_FOLDER,
                                               ImGui::GetColorU32(colors.yellow));

    // Context menu for schema
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            // schemaData implements IDatabaseNode, so we can pass it directly
            app.getTabManager()->createSQLEditorTab("", schemaData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(schemaData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            schemaData->startTablesLoadAsync(true);
            schemaData->startViewsLoadAsync(true);
            schemaData->startMaterializedViewsLoadAsync(true);
            schemaData->startSequencesLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = schemaData->name;
            InputDialog::instance().showWithValidation(
                "Rename Schema", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [schemaData](const std::string& newName) {
                    auto [success, error] = schemaData->renameSchema(newName);
                    if (!success) {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            Alert::show(
                "Delete Schema",
                std::format("Permanently delete '{}' and ALL its contents? This is irreversible.",
                            schemaData->name),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [schemaData]() {
                      auto [success, error] = schemaData->dropSchema();
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete schema: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // Render Tables section
        {
            const std::string tablesNodeId = std::format("tables_{}_{:p}", schemaData->name,
                                                         static_cast<void*>(&schemaData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            // Context menu for Tables node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    TableDialog::instance().showCreate(schemaData, schemaData->name);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!schemaData->tablesLoaded && !schemaData->tablesLoader.isRunning()) {
                    schemaData->startTablesLoadAsync();
                }

                if (schemaData->tablesLoader.isRunning()) {
                    schemaData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading tables...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->tablesLoaded) {
                    if (schemaData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : schemaData->tables) {
                            renderTableNode(table, schemaData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Views section
        {
            const std::string viewsNodeId = std::format("views_{}_{:p}", schemaData->name,
                                                        static_cast<void*>(&schemaData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            // Context menu for Views node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startViewsLoadAsync(true); // Force refresh
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!schemaData->viewsLoaded && !schemaData->viewsLoader.isRunning()) {
                    schemaData->startViewsLoadAsync();
                }

                if (schemaData->viewsLoader.isRunning()) {
                    schemaData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading views...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->viewsLoaded) {
                    if (schemaData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : schemaData->views) {
                            renderViewNode(view, schemaData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Materialized Views section
        {
            const std::string matViewsNodeId =
                std::format("matviews_{}_{:p}", schemaData->name,
                            static_cast<void*>(&schemaData->materializedViews));
            const bool matViewsOpen =
                renderTreeNodeWithIcon("Materialized Views", matViewsNodeId, ICON_FK_EYE,
                                       ImGui::GetColorU32(colors.lavender));

            // Context menu for Materialized Views node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startMaterializedViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (matViewsOpen) {
                if (!schemaData->materializedViewsLoaded &&
                    !schemaData->materializedViewsLoader.isRunning()) {
                    schemaData->startMaterializedViewsLoadAsync();
                }

                if (schemaData->materializedViewsLoader.isRunning()) {
                    schemaData->checkMaterializedViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading materialized views...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_matviews", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->materializedViewsLoaded) {
                    if (schemaData->materializedViews.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No materialized views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& mv : schemaData->materializedViews) {
                            renderViewNode(mv, schemaData, true);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Sequences section
        {
            const std::string seqNodeId = std::format("sequences_{}_{:p}", schemaData->name,
                                                      static_cast<void*>(&schemaData->sequences));
            const bool seqOpen = renderTreeNodeWithIcon(
                "Sequences", seqNodeId, ICON_FK_SORT_NUMERIC_ASC, ImGui::GetColorU32(colors.mauve));

            // Context menu for Sequences node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startSequencesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (seqOpen) {
                if (!schemaData->sequencesLoaded && !schemaData->sequencesLoader.isRunning()) {
                    schemaData->startSequencesLoadAsync();
                }

                if (schemaData->sequencesLoader.isRunning()) {
                    schemaData->checkSequencesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading sequences...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_sequences", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->sequencesLoaded) {
                    if (schemaData->sequences.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No sequences");
                        ImGui::PopStyleColor();
                    } else {
                        constexpr ImGuiTreeNodeFlags seqFlags =
                            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                            ImGuiTreeNodeFlags_FramePadding;
                        for (const auto& seq : schemaData->sequences) {
                            const std::string seqItemId =
                                std::format("seq_{}_{:p}", seq, static_cast<const void*>(&seq));
                            const std::string seqLabel = std::format("   {}###{}", seq, seqItemId);
                            ImGui::TreeNodeEx(seqLabel.c_str(), seqFlags);

                            const auto iconPos = ImVec2(
                                ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                                ImGui::GetItemRectMin().y +
                                    (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) *
                                        0.5f);
                            ImGui::GetWindowDrawList()->AddText(iconPos,
                                                                ImGui::GetColorU32(colors.mauve),
                                                                ICON_FK_SORT_NUMERIC_ASC);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMySQLDatabaseNode(MySQLDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.blue));

    // Handle expand/collapse
    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = dbData->name;
            InputDialog::instance().showWithValidation(
                "Rename Database", "New name:", oldName, "Rename",
                [](const std::string&) -> std::string {
                    return "MySQL does not support direct database renaming. You need to create a "
                           "new database, copy all data, and drop the old one.";
                },
                [](const std::string&) {});
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [this, dbName]() {
                      auto [success, error] = db->dropDatabase(dbName);
                      if (success) {
                          Logger::info(std::format("Database '{}' deleted successfully", dbName));
                          if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
                              mysqlDb->refreshDatabaseNames();
                          }
                      } else {
                          Logger::error(std::format("Failed to delete database: {}", error));
                          Alert::show("Error", std::format("Failed to delete database: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // Render Tables section
        {
            const std::string tablesNodeId =
                std::format("tables_{}_{:p}", dbData->name, static_cast<void*>(&dbData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            // Context menu for Tables node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    TableDialog::instance().showCreate(dbData);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!dbData->tablesLoaded && !dbData->tablesLoader.isRunning()) {
                    dbData->startTablesLoadAsync();
                }

                if (dbData->tablesLoader.isRunning()) {
                    dbData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading tables...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->tablesLoaded) {
                    if (dbData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : dbData->tables) {
                            renderMySQLTableNode(table, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Views section
        {
            const std::string viewsNodeId =
                std::format("views_{}_{:p}", dbData->name, static_cast<void*>(&dbData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            // Context menu for Views node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!dbData->viewsLoaded && !dbData->viewsLoader.isRunning()) {
                    dbData->startViewsLoadAsync();
                }

                if (dbData->viewsLoader.isRunning()) {
                    dbData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading views...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->viewsLoaded) {
                    if (dbData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : dbData->views) {
                            renderMySQLViewNode(view, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderTableNode(Table& table, PostgresSchemaNode* schemaNode) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string tableNodeId =
        std::format("pg_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    // Check if table is refreshing
    const bool isRefreshing = schemaNode->isTableRefreshing(table.name);

    // Show loading indicator if refreshing
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        schemaNode->checkTableRefreshStatusAsync(table.name);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaNode, table.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(schemaNode, table.name);
        }
        if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
            TableDialog::instance().showEdit(schemaNode, table, schemaNode->name);
        }
        if (ImGui::MenuItem(SHOW_STRUCTURE_LABEL)) {
            // TODO: Show table structure in a tab
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            schemaNode->startTableRefreshAsync(table.name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = table.name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [schemaNode, oldName](const std::string& newName) {
                    auto [success, error] = schemaNode->renameTable(oldName, newName);
                    if (!success) {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string tableName = table.name;
            Alert::show("Delete Table",
                        std::format("Permanently delete '{}.{}'? This is irreversible.",
                                    schemaNode->name, tableName),
                        {{"Cancel", nullptr, AlertButton::Style::Cancel},
                         {"Delete",
                          [schemaNode, tableName]() {
                              auto [success, error] = schemaNode->dropTable(tableName);
                              if (!success) {
                                  Alert::show("Error",
                                              std::format("Failed to delete table: {}", error));
                              }
                          },
                          AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // Columns section (PostgreSQL)
        {
            const std::string columnsNodeId =
                std::format("pg_columns_{}_{:p}", table.name, static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (column.isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }

                    const std::string columnNodeId =
                        std::format("pg_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    // Context menu for column
                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}.{}'?", schemaNode->name,
                                            tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [schemaNode, tblName, colName]() {
                                      auto [success, error] =
                                          schemaNode->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("pg_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                            fk.targetTable, fk.targetColumn);
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                        if (ImGui::IsItemHovered() && !fk.name.empty()) {
                            ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No foreign keys");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // References section (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                    if (ImGui::IsItemHovered()) {
                        std::string tooltip =
                            std::format("{}.{} -> {}.{}", ref.targetTable, ref.sourceColumn,
                                        table.name, ref.targetColumn);
                        if (!ref.name.empty()) {
                            tooltip += std::format("\nConstraint: {}", ref.name);
                        }
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderViewNode(Table& view, PostgresSchemaNode* schemaData,
                                       bool isMaterializedView) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    // Draw icon
    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaData, view.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(schemaData, view.name);
        }
        if (ImGui::MenuItem(SHOW_STRUCTURE_LABEL)) {
            // TODO: Show view structure in a tab
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string viewName = view.name;
            const std::string typeLabel = isMaterializedView ? "Materialized View" : "View";
            Alert::show(
                std::format("Delete {}", typeLabel),
                std::format("Permanently delete {} '{}.{}'? This is irreversible.", typeLabel,
                            schemaData->name, viewName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [schemaData, viewName, isMaterializedView]() {
                      auto [success, error] = schemaData->dropView(viewName, isMaterializedView);
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete view: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string tableNodeId =
        std::format("mysql_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    // Check if table is refreshing
    const bool isRefreshing = dbData->isTableRefreshing(table.name);

    // Show loading indicator if refreshing
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        dbData->checkTableRefreshStatusAsync(table.name);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, table.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, table.name);
        }
        if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
            TableDialog::instance().showEdit(dbData, table);
        }
        if (ImGui::MenuItem(SHOW_STRUCTURE_LABEL)) {
            // TODO: Show table structure in a tab
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTableRefreshAsync(table.name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = table.name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [dbData, oldName](const std::string& newName) {
                    auto [success, error] = dbData->renameTable(oldName, newName);
                    if (!success) {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string tableName = table.name;
            Alert::show(
                "Delete Table",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            tableName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [dbData, tableName]() {
                      auto [success, error] = dbData->dropTable(tableName);
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete table: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // Columns section (MySQL)
        {
            const std::string columnsNodeId = std::format("mysql_columns_{}_{:p}", table.name,
                                                          static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (column.isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }

                    const std::string columnNodeId =
                        std::format("mysql_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    // Context menu for column (MySQL)
                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [dbData, tblName, colName]() {
                                      auto [success, error] = dbData->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section (MySQL)
        {
            const std::string fkNodeId =
                std::format("mysql_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                            fk.targetTable, fk.targetColumn);
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                        if (ImGui::IsItemHovered() && !fk.name.empty()) {
                            ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No foreign keys");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // References section (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                    if (ImGui::IsItemHovered()) {
                        std::string tooltip =
                            std::format("{}.{} -> {}.{}", ref.targetTable, ref.sourceColumn,
                                        table.name, ref.targetColumn);
                        if (!ref.name.empty()) {
                            tooltip += std::format("\nConstraint: {}", ref.name);
                        }
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMySQLViewNode(Table& view, MySQLDatabaseNode* dbData) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    // Draw icon
    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, view.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, view.name);
        }
        if (ImGui::MenuItem(SHOW_STRUCTURE_LABEL)) {
            // TODO: Show view structure in a tab
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMSSQLDatabaseNode(MSSQLDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.mauve));

    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [this, dbName]() {
                      auto [success, error] = db->dropDatabase(dbName);
                      if (success) {
                          Logger::info(std::format("Database '{}' deleted successfully", dbName));
                          if (auto* mssqlDb = dynamic_cast<MSSQLDatabase*>(db.get())) {
                              mssqlDb->refreshDatabaseNames();
                          }
                      } else {
                          Logger::error(std::format("Failed to delete database: {}", error));
                          Alert::show("Error", std::format("Failed to delete database: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // tables
        {
            const std::string tablesNodeId =
                std::format("tables_{}_{:p}", dbData->name, static_cast<void*>(&dbData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    TableDialog::instance().showCreate(dbData);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!dbData->tablesLoaded && !dbData->tablesLoader.isRunning()) {
                    dbData->startTablesLoadAsync();
                }

                if (dbData->tablesLoader.isRunning()) {
                    dbData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading tables...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->tablesLoaded) {
                    if (dbData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : dbData->tables) {
                            renderMSSQLTableNode(table, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // views
        {
            const std::string viewsNodeId =
                std::format("views_{}_{:p}", dbData->name, static_cast<void*>(&dbData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!dbData->viewsLoaded && !dbData->viewsLoader.isRunning()) {
                    dbData->startViewsLoadAsync();
                }

                if (dbData->viewsLoader.isRunning()) {
                    dbData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading views...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->viewsLoaded) {
                    if (dbData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : dbData->views) {
                            renderMSSQLViewNode(view, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMSSQLTableNode(Table& table, MSSQLDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string tableNodeId =
        std::format("mssql_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    const bool isRefreshing = dbData->isTableRefreshing(table.name);
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        dbData->checkTableRefreshStatusAsync(table.name);
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, table.name);
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, table.name);
        }
        if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
            TableDialog::instance().showEdit(dbData, table);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTableRefreshAsync(table.name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = table.name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [dbData, oldName](const std::string& newName) {
                    auto [success, error] = dbData->renameTable(oldName, newName);
                    if (!success) {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string tableName = table.name;
            Alert::show(
                "Delete Table",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            tableName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [dbData, tableName]() {
                      auto [success, error] = dbData->dropTable(tableName);
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete table: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // columns
        {
            const std::string columnsNodeId = std::format("mssql_columns_{}_{:p}", table.name,
                                                          static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey)
                        columnDisplay += ", PK";
                    if (column.isNotNull)
                        columnDisplay += ", NOT NULL";

                    const std::string columnNodeId =
                        std::format("mssql_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [dbData, tblName, colName]() {
                                      auto [success, error] = dbData->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // foreign keys
        if (!table.foreignKeys.empty()) {
            const std::string fkNodeId =
                std::format("mssql_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                for (const auto& fk : table.foreignKeys) {
                    ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;
                    std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                        fk.targetTable, fk.targetColumn);
                    ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                    if (ImGui::IsItemHovered() && !fk.name.empty()) {
                        ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        // indexes
        if (!table.indexes.empty()) {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                for (const auto& index : table.indexes) {
                    ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_FramePadding;
                    std::string indexDisplay = index.name;
                    if (!index.columns.empty()) {
                        indexDisplay += " (";
                        for (size_t i = 0; i < index.columns.size(); ++i) {
                            if (i > 0)
                                indexDisplay += ", ";
                            indexDisplay += index.columns[i];
                        }
                        indexDisplay += ")";
                    }
                    if (index.isUnique)
                        indexDisplay += " UNIQUE";
                    ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                }
                ImGui::TreePop();
            }
        }

        // references (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMSSQLViewNode(Table& view, MSSQLDatabaseNode* dbData) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, view.name);
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, view.name);
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMongoDBDatabaseNode(MongoDBDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.green));

    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startCollectionsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [this, dbName]() {
                      auto [success, error] = db->dropDatabase(dbName);
                      if (success) {
                          Logger::info(std::format("Database '{}' deleted successfully", dbName));
                          if (auto* mongoDb = dynamic_cast<MongoDBDatabase*>(db.get())) {
                              mongoDb->refreshDatabaseNames();
                          }
                      } else {
                          Logger::error(std::format("Failed to delete database: {}", error));
                          Alert::show("Error", std::format("Failed to delete database: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // Render Collections section
        {
            const std::string collectionsNodeId = std::format(
                "collections_{}_{:p}", dbData->name, static_cast<void*>(&dbData->collections));
            const bool collectionsOpen = renderTreeNodeWithIcon(
                "Collections", collectionsNodeId, ICON_FK_TABLE, ImGui::GetColorU32(colors.green));

            // Context menu for Collections node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startCollectionsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (collectionsOpen) {
                if (!dbData->collectionsLoaded && !dbData->collectionsLoader.isRunning()) {
                    dbData->startCollectionsLoadAsync();
                }

                if (dbData->collectionsLoader.isRunning()) {
                    dbData->checkCollectionsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::Text("  Loading collections...");
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_collections", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->collectionsLoaded) {
                    if (dbData->collections.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No collections");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& collection : dbData->collections) {
                            renderMongoDBCollectionNode(collection, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMongoDBCollectionNode(Table& collection,
                                                    MongoDBDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags collectionFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string collectionNodeId =
        std::format("mongo_coll_{}_{:p}", collection.name, static_cast<const void*>(&collection));
    const bool collectionOpen =
        renderTreeNodeWithIcon(collection.name, collectionNodeId, ICON_FK_TABLE,
                               ImGui::GetColorU32(colors.green), collectionFlags);

    // Check if collection is refreshing
    const bool isRefreshing = dbData->isTableRefreshing(collection.name);

    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_coll_{}", collection.name).c_str(),
                         spinnerRadius, 2, ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        dbData->checkTableRefreshStatusAsync(collection.name);
    }

    // Double-click to open collection viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, collection.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, collection.name);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTableRefreshAsync(collection.name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string collName = collection.name;
            Alert::show(
                "Delete Collection",
                std::format("Permanently delete '{}' and ALL its documents? This is irreversible.",
                            collName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [dbData, collName]() {
                      auto [success, error] = dbData->dropCollection(collName);
                      if (!success) {
                          Alert::show("Error",
                                      std::format("Failed to delete collection: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (collectionOpen) {
        // Fields section (inferred schema)
        {
            const std::string fieldsNodeId = std::format("mongo_fields_{}_{:p}", collection.name,
                                                         static_cast<void*>(&collection.columns));
            const bool fieldsOpen = renderTreeNodeWithIcon(
                "Fields", fieldsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (fieldsOpen) {
                if (collection.columns.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No fields detected");
                    ImGui::PopStyleColor();
                } else {
                    for (const auto& column : collection.columns) {
                        ImGuiTreeNodeFlags fieldFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;

                        std::string fieldDisplay = std::format("{} ({})", column.name, column.type);
                        if (column.isPrimaryKey) {
                            fieldDisplay += ", PK";
                        }

                        const std::string fieldNodeId =
                            std::format("mongo_field_{}_{}_{:p}", collection.name, column.name,
                                        static_cast<const void*>(&column));
                        const std::string fieldLabel =
                            std::format("{}###{}", fieldDisplay, fieldNodeId);
                        ImGui::TreeNodeEx(fieldLabel.c_str(), fieldFlags);
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderSQLiteTableNode(Table& table, SQLiteDatabase* sqliteDb) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string tableNodeId =
        std::format("sqlite_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(sqliteDb, table.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(sqliteDb, table.name);
        }
        if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
            TableDialog::instance().showEdit(sqliteDb, table);
        }
        if (ImGui::MenuItem(SHOW_STRUCTURE_LABEL)) {
            // TODO: Show table structure in a tab
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = table.name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [sqliteDb, oldName](const std::string& newName) {
                    auto [success, error] = sqliteDb->renameTable(oldName, newName);
                    if (!success) {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string tableName = table.name;
            Alert::show(
                "Delete Table",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            tableName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [sqliteDb, tableName]() {
                      auto [success, error] = sqliteDb->dropTable(tableName);
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete table: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // Columns section
        {
            const std::string columnsNodeId =
                std::format("columns_{}_{:p}", table.name, static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (column.isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }

                    const std::string columnNodeId =
                        std::format("sqlite_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    // Context menu for column
                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [sqliteDb, tblName, colName]() {
                                      auto [success, error] =
                                          sqliteDb->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("sqlite_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                            fk.targetTable, fk.targetColumn);
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                        if (ImGui::IsItemHovered() && !fk.name.empty()) {
                            ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No foreign keys");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // References section (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                    if (ImGui::IsItemHovered()) {
                        std::string tooltip =
                            std::format("{}.{} -> {}.{}", ref.targetTable, ref.sourceColumn,
                                        table.name, ref.targetColumn);
                        if (!ref.name.empty()) {
                            tooltip += std::format("\nConstraint: {}", ref.name);
                        }
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderSQLiteViewNode(Table& view, SQLiteDatabase* sqliteDb) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    // Draw icon
    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(sqliteDb, view.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(sqliteDb, view.name);
        }
        if (ImGui::MenuItem(SHOW_STRUCTURE_LABEL)) {
            // TODO: Show view structure in a tab
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}
