#include "ui/new/database_node.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "ui/confirm_dialog.hpp"
#include "ui/input_dialog.hpp"
#include "ui/table_dialog.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <format>
#include <ranges>

namespace NewHierarchy {
    // Forward declaration for dialog access
    TableDialog& getTableDialog();
} // namespace NewHierarchy

// ==================== DatabaseHierarchy Class Implementation ====================

DatabaseHierarchy::DatabaseHierarchy(std::shared_ptr<DatabaseInterface> dbInterface)
    : db(std::move(dbInterface)) {}

bool DatabaseHierarchy::renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                                               const std::string& icon, ImU32 iconColor,
                                               ImGuiTreeNodeFlags flags) {
    const std::string fullLabel = std::format("   {}###{}", label, nodeId);
    const bool isOpen = ImGui::TreeNodeEx(fullLabel.c_str(), flags);

    // Draw icon
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

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    // Get database type
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
            ImGui::SameLine();
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
    } else if (dbType == DatabaseType::MYSQL) {
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
            ImGui::SameLine();
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
    } else if (dbType == DatabaseType::REDIS) {
        auto redisDb = std::dynamic_pointer_cast<RedisDatabase>(db);
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

        // Show Redis connection info
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), ICON_FA_DATABASE " Connected");

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

                const std::string displayName = (keyGroup.name == "*") ? "All Keys" : keyGroup.name;
                const std::string keyGroupId = std::format("redis_key_{}_{:p}", displayName,
                                                           static_cast<const void*>(&keyGroup));

                ImGui::TreeNodeEx(keyGroupId.c_str(), keyGroupFlags, "%s", displayName.c_str());
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), ICON_FA_KEY);

                // Context menu
                if (ImGui::BeginPopupContextItem(keyGroupId.c_str())) {
                    if (ImGui::MenuItem("Refresh Keys")) {
                        redisDb->startKeysLoadAsync(true);
                    }
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
            if (ImGui::MenuItem("Refresh")) {
                sqliteDb->startTablesLoadAsync();
            }
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
                ImGui::SameLine();
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
            if (ImGui::MenuItem("Refresh")) {
                sqliteDb->startViewsLoadAsync();
            }
            ImGui::EndPopup();
        }

        if (viewsOpen) {
            if (!sqliteDb->loadingViews.load() && sqliteDb->getViews().empty()) {
                sqliteDb->startViewsLoadAsync();
            }

            if (sqliteDb->loadingViews) {
                sqliteDb->checkViewsStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::Text("  Loading views...");
                ImGui::SameLine();
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
        if (ImGui::MenuItem("New SQL Editor")) {
            // Default to first schema if available
            std::string defaultSchema;
            if (dbData->schemasLoaded && !dbData->schemas.empty()) {
                defaultSchema = dbData->schemas[0]->name;
            }
            app.getTabManager()->createSQLEditorTab("", dbData, defaultSchema);
        }
        if (ImGui::MenuItem("Refresh")) {
            dbData->startSchemasLoadAsync(true, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
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
        if (ImGui::MenuItem("Delete...")) {
            const std::string dbName = dbData->name;
            ConfirmDialog::instance().show(
                "Delete Database", std::format("You are about to delete the database: {}", dbName),
                {"Permanently delete the database and ALL its data",
                 "Remove all tables, views, and other objects", "This operation is IRREVERSIBLE"},
                "Delete Database", [this, dbName]() {
                    auto [success, error] = db->dropDatabase(dbName);
                    if (success) {
                        Logger::info(std::format("Database '{}' deleted successfully", dbName));
                        if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                            pgDb->refreshDatabaseNames();
                        }
                    } else {
                        Logger::error(std::format("Failed to delete database: {}", error));
                        ConfirmDialog::instance().setError(error);
                    }
                });
        }
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
            ImGui::SameLine();
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
        if (ImGui::MenuItem("New SQL Editor")) {
            app.getTabManager()->createSQLEditorTab("", schemaData->parentDbNode, schemaData->name);
        }
        if (ImGui::MenuItem("Show Diagram")) {
            app.getTabManager()->createDiagramTab(schemaData);
        }
        if (ImGui::MenuItem("Refresh")) {
            schemaData->startTablesLoadAsync(true);
            schemaData->startViewsLoadAsync(true);
            schemaData->startSequencesLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            const std::string oldName = schemaData->name;
            InputDialog::instance().showWithValidation(
                "Rename Schema", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [schemaData, oldName](const std::string& newName) {
                    const std::string sql =
                        std::format("ALTER SCHEMA \"{}\" RENAME TO \"{}\"", oldName, newName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = schemaData->executeQuery(sql);
                    if (success) {
                        if (schemaData->parentDbNode) {
                            schemaData->parentDbNode->startSchemasLoadAsync(true, false);
                        }
                    } else {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem("Delete...")) {
            const std::string schemaName = schemaData->name;
            ConfirmDialog::instance().show(
                "Delete Schema", std::format("You are about to delete the schema: {}", schemaName),
                {"Permanently delete the schema and ALL its contents",
                 "Remove all tables, views, sequences in this schema",
                 "This operation is IRREVERSIBLE"},
                "Delete Schema", [schemaData, schemaName]() {
                    const std::string sql = std::format("DROP SCHEMA \"{}\" CASCADE", schemaName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = schemaData->executeQuery(sql);
                    if (success && schemaData->parentDbNode) {
                        schemaData->parentDbNode->startSchemasLoadAsync(true, false);
                    } else if (!success) {
                        ConfirmDialog::instance().setError(error);
                    }
                });
        }
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
                if (ImGui::MenuItem("Refresh")) {
                    schemaData->startTablesLoadAsync(true);
                }
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
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->tablesLoaded) {
                    if (schemaData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : schemaData->tables) {
                            renderTableNode(table, schemaData, dbData->name, schemaData->name);
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
                if (ImGui::MenuItem("Refresh")) {
                    schemaData->startViewsLoadAsync(true); // Force refresh
                }
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
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->viewsLoaded) {
                    if (schemaData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : schemaData->views) {
                            renderViewNode(view, schemaData, dbData->name, schemaData->name);
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
                if (ImGui::MenuItem("Refresh")) {
                    schemaData->startSequencesLoadAsync(true);
                }
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
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_sequences", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->sequencesLoaded) {
                    if (schemaData->sequences.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No sequences");
                        ImGui::PopStyleColor();
                    } else {
                        for (const auto& seq : schemaData->sequences) {
                            ImGui::Text("    %s", seq.c_str());
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
        if (ImGui::MenuItem("New SQL Editor")) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem("Show Diagram")) {
            app.getTabManager()->createDiagramTab(dbData);
        }
        if (ImGui::MenuItem("Refresh")) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            // MySQL doesn't support direct database renaming
            const std::string oldName = dbData->name;
            InputDialog::instance().showWithValidation(
                "Rename Database", "New name:", oldName, "Rename",
                [](const std::string&) -> std::string {
                    return "MySQL does not support direct database renaming. You need to create a "
                           "new database, copy all data, and drop the old one.";
                },
                [](const std::string&) {
                    // This won't be called due to validation always failing
                });
        }
        if (ImGui::MenuItem("Delete...")) {
            const std::string dbName = dbData->name;
            ConfirmDialog::instance().show(
                "Delete Database", std::format("You are about to delete the database: {}", dbName),
                {"Permanently delete the database and ALL its data",
                 "Remove all tables, views, and other objects", "This operation is IRREVERSIBLE"},
                "Delete Database", [this, dbName]() {
                    auto [success, error] = db->dropDatabase(dbName);
                    if (success) {
                        Logger::info(std::format("Database '{}' deleted successfully", dbName));
                        if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
                            mysqlDb->refreshDatabaseNames();
                        }
                    } else {
                        Logger::error(std::format("Failed to delete database: {}", error));
                        ConfirmDialog::instance().setError(error);
                    }
                });
        }
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
                if (ImGui::MenuItem("Refresh")) {
                    dbData->startTablesLoadAsync(true);
                }
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
                    ImGui::SameLine();
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
                if (ImGui::MenuItem("Refresh")) {
                    dbData->startViewsLoadAsync(true);
                }
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
                    ImGui::SameLine();
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

void DatabaseHierarchy::renderTableNode(Table& table, PostgresSchemaNode* schemaData,
                                        const std::string& databaseName,
                                        const std::string& schemaName) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    if (table.expanded) {
        tableFlags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    const std::string tableNodeId =
        std::format("table_{}_{:p}", table.name, static_cast<void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    table.expanded = tableOpen;

    // Check if table is refreshing
    const bool isRefreshing = schemaData->isTableRefreshing(table.name);

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

        schemaData->checkTableRefreshStatusAsync(table.name);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaData, table.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(schemaData, table.name);
        }
        if (ImGui::MenuItem("Edit Table")) {
            NewHierarchy::getTableDialog().showTableDialog(schemaData, table.name, schemaName);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show table structure in a tab
        }
        if (ImGui::MenuItem("Refresh")) {
            schemaData->startTableRefreshAsync(table.name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            const std::string oldName = table.name;
            const std::string schemaNameCopy = schemaData->name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [schemaData, schemaNameCopy, oldName](const std::string& newName) {
                    const std::string sql =
                        std::format("ALTER TABLE \"{}\".\"{}\" RENAME TO \"{}\"", schemaNameCopy,
                                    oldName, newName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = schemaData->executeQuery(sql);
                    if (success) {
                        schemaData->startTablesLoadAsync(true);
                    } else {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem("Delete...")) {
            const std::string tableName = table.name;
            const std::string schemaNameCopy = schemaData->name;
            ConfirmDialog::instance().show(
                "Delete Table",
                std::format("You are about to delete the table: {}.{}", schemaNameCopy, tableName),
                {"Permanently delete the table and ALL its data",
                 "Remove all indexes and constraints",
                 "Break any foreign key references to this table"},
                "Delete Table", [schemaData, schemaNameCopy, tableName]() {
                    const std::string sql =
                        std::format("DROP TABLE \"{}\".\"{}\"", schemaNameCopy, tableName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = schemaData->executeQuery(sql);
                    if (success) {
                        schemaData->startTablesLoadAsync(true);
                    } else {
                        ConfirmDialog::instance().setError(error);
                    }
                });
        }
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
                        std::format("col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("   {}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("   {} -> {}.{}", fk.sourceColumn,
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
                        std::string indexDisplay = "   " + index.name;
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
                        std::format("   {}.{}", ref.targetTable, ref.sourceColumn);
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
                                       const std::string& databaseName,
                                       const std::string& schemaName) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
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
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(schemaData, view.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show view structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    if (table.expanded) {
        tableFlags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    const std::string tableNodeId =
        std::format("table_{}_{:p}", table.name, static_cast<void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    table.expanded = tableOpen;

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
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(dbData, table.name);
        }
        if (ImGui::MenuItem("Edit Table")) {
            NewHierarchy::getTableDialog().showTableDialog(dbData, table.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show table structure in a tab
        }
        if (ImGui::MenuItem("Refresh")) {
            dbData->startTableRefreshAsync(table.name);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            const std::string oldName = table.name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [dbData, oldName](const std::string& newName) {
                    const std::string sql =
                        std::format("RENAME TABLE `{}` TO `{}`", oldName, newName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = dbData->executeQuery(sql);
                    if (success) {
                        dbData->startTablesLoadAsync(true);
                    } else {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem("Delete...")) {
            const std::string tableName = table.name;
            ConfirmDialog::instance().show(
                "Delete Table", std::format("You are about to delete the table: {}", tableName),
                {"Permanently delete the table and ALL its data",
                 "Remove all indexes and constraints",
                 "Break any foreign key references to this table"},
                "Delete Table", [dbData, tableName]() {
                    const std::string sql = std::format("DROP TABLE `{}`", tableName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = dbData->executeQuery(sql);
                    if (success) {
                        dbData->startTablesLoadAsync(true);
                    } else {
                        ConfirmDialog::instance().setError(error);
                    }
                });
        }
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
                        std::format("col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("   {}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("   {} -> {}.{}", fk.sourceColumn,
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
                        std::string indexDisplay = "   " + index.name;
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
                        std::format("   {}.{}", ref.targetTable, ref.sourceColumn);
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
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
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
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(dbData, view.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show view structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderSQLiteTableNode(Table& table, SQLiteDatabase* sqliteDb) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    if (table.expanded) {
        tableFlags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    const std::string tableNodeId =
        std::format("table_{}_{:p}", table.name, static_cast<void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    table.expanded = tableOpen;

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(sqliteDb, table.name);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(sqliteDb, table.name);
        }
        if (ImGui::MenuItem("Edit Table")) {
            NewHierarchy::getTableDialog().showTableDialog(sqliteDb, table.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show table structure in a tab
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            const std::string oldName = table.name;
            InputDialog::instance().showWithValidation(
                "Rename Table", "New name:", oldName, "Rename",
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                },
                [sqliteDb, oldName](const std::string& newName) {
                    const std::string sql =
                        std::format("ALTER TABLE \"{}\" RENAME TO \"{}\"", oldName, newName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = sqliteDb->executeQuery(sql);
                    if (success) {
                        sqliteDb->startTablesLoadAsync(true);
                    } else {
                        InputDialog::instance().setError(error);
                    }
                });
        }
        if (ImGui::MenuItem("Delete...")) {
            const std::string tableName = table.name;
            ConfirmDialog::instance().show(
                "Delete", std::format("You are about to delete the table: {}", tableName),
                {"Permanently delete the table and ALL its data",
                 "Remove all indexes and constraints",
                 "Break any foreign key references to this table"},
                "Delete Table", [sqliteDb, tableName]() {
                    const std::string sql = std::format("DROP TABLE \"{}\"", tableName);
                    Logger::info("Executing: " + sql);
                    auto [success, error] = sqliteDb->executeQuery(sql);
                    if (success) {
                        sqliteDb->startTablesLoadAsync(true);
                    } else {
                        ConfirmDialog::instance().setError(error);
                    }
                });
        }
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
                        std::format("col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("   {}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("   {} -> {}.{}", fk.sourceColumn,
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
                        std::string indexDisplay = "   " + index.name;
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
                        std::format("   {}.{}", ref.targetTable, ref.sourceColumn);
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
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
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
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(sqliteDb, view.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show view structure in a tab
        }
        ImGui::EndPopup();
    }
}
