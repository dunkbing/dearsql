#include "ui/new/database_node.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "imgui.h"
#include "utils/spinner.hpp"
#include <format>
#include <ranges>

namespace NewHierarchy {
    namespace {
        // helper function to render a tree node with icon
        bool
        renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                               const std::string& icon, ImU32 iconColor,
                               ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                          ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                          ImGuiTreeNodeFlags_FramePadding) {
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
    } // namespace

    void renderRootDatabaseNode(const std::shared_ptr<DatabaseInterface>& dbInterface) {
        if (!dbInterface) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        // Get database type
        const auto dbType = dbInterface->getType();

        if (dbType == DatabaseType::SQLITE) {
            // SQLite: direct tables/views rendering (no multi-database support)
            // TODO: Implement SQLite hierarchy
            ImGui::Text("SQLite rendering");
        } else if (dbType == DatabaseType::POSTGRESQL) {
            auto* pgDb = dynamic_cast<PostgresDatabase*>(dbInterface.get());
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
                UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (pgDb->areDatabasesLoaded()) {
                const auto& databases = pgDb->getDatabaseDataMap() | std::views::values;
                for (const auto& dbDataPtr : databases) {
                    if (dbDataPtr) {
                        renderPostgresDatabaseNode(pgDb, dbDataPtr.get());
                    }
                }
            }
        } else if (dbType == DatabaseType::MYSQL) {
            auto* mysqlDb = dynamic_cast<MySQLDatabase*>(dbInterface.get());
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
                UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (mysqlDb->areDatabasesLoaded()) {
                const auto& databases = mysqlDb->getDatabaseDataMap() | std::views::values;
                for (const auto& dbDataPtr : databases) {
                    if (dbDataPtr) {
                        renderMySQLDatabaseNode(mysqlDb, dbDataPtr.get());
                    }
                }
            }
        }
    }

    // Database-specific rendering implementations
    void renderPostgresDatabaseNode(PostgresDatabase* pgDb, PostgresDatabaseNode* dbData) {
        if (!pgDb || !dbData) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        const std::string nodeId =
            std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
        const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                                   ImGui::GetColorU32(colors.blue));

        // Handle expand/collapse
        if (ImGui::IsItemToggledOpen()) {
            dbData->expanded = isOpen;
        }

        // Context menu
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("Refresh")) {
                dbData->startSchemasLoadAsync(true);
            }
            ImGui::EndPopup();
        }

        if (isOpen) {
            // PostgreSQL: render schemas
            if (!dbData->schemasLoaded && !dbData->loadingSchemas) {
                dbData->startSchemasLoadAsync();
            }

            if (dbData->loadingSchemas) {
                dbData->checkSchemasStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::Text("  Loading schemas...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_schemas", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (dbData->schemasLoaded) {
                // Render each schema
                for (auto& schema : dbData->schemas) {
                    renderPostgresSchemaNode(pgDb, dbData, schema.get());
                }
            }

            ImGui::TreePop();
        }
    }

    void renderPostgresSchemaNode(PostgresDatabase* pgDb, PostgresDatabaseNode* dbData,
                                  PostgresSchemaNode* schemaData) {
        if (!pgDb || !dbData || !schemaData) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        // Get the shared_ptr for the database
        const auto& databases = app.getDatabases();
        std::shared_ptr<DatabaseInterface> dbInterface;
        for (const auto& db : databases) {
            if (db.get() == static_cast<DatabaseInterface*>(pgDb)) {
                dbInterface = db;
                break;
            }
        }

        if (!dbInterface) {
            return;
        }

        const std::string nodeId =
            std::format("schema_{}_{:p}", schemaData->name, static_cast<void*>(schemaData));
        const bool isOpen = renderTreeNodeWithIcon(schemaData->name, nodeId, ICON_FK_FOLDER,
                                                   ImGui::GetColorU32(colors.yellow));

        if (isOpen) {
            // Render Tables section
            {
                const std::string tablesNodeId = std::format(
                    "tables_{}_{:p}", schemaData->name, static_cast<void*>(&schemaData->tables));
                const bool tablesOpen = renderTreeNodeWithIcon(
                    "Tables", tablesNodeId, ICON_FK_TABLE, ImGui::GetColorU32(colors.green));

                // Context menu for Tables node
                if (ImGui::BeginPopupContextItem(nullptr)) {
                    if (ImGui::MenuItem("Refresh")) {
                        schemaData->startTablesLoadAsync(true); // Force refresh
                    }
                    ImGui::EndPopup();
                }

                if (tablesOpen) {
                    if (!schemaData->tablesLoaded && !schemaData->loadingTables) {
                        schemaData->startTablesLoadAsync();
                    }

                    if (schemaData->loadingTables) {
                        schemaData->checkTablesStatusAsync();
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading tables...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_tables", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
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
                    if (!schemaData->viewsLoaded && !schemaData->loadingViews) {
                        schemaData->startViewsLoadAsync();
                    }

                    if (schemaData->loadingViews) {
                        schemaData->checkViewsStatusAsync();
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading views...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_views", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
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
                const std::string seqNodeId =
                    std::format("sequences_{}_{:p}", schemaData->name,
                                static_cast<void*>(&schemaData->sequences));
                const bool seqOpen =
                    renderTreeNodeWithIcon("Sequences", seqNodeId, ICON_FK_SORT_NUMERIC_ASC,
                                           ImGui::GetColorU32(colors.mauve));

                // Context menu for Sequences node
                if (ImGui::BeginPopupContextItem(nullptr)) {
                    if (ImGui::MenuItem("Refresh")) {
                        schemaData->startSequencesLoadAsync(true); // Force refresh
                    }
                    ImGui::EndPopup();
                }

                if (seqOpen) {
                    if (!schemaData->sequencesLoaded && !schemaData->loadingSequences) {
                        schemaData->startSequencesLoadAsync();
                    }

                    if (schemaData->loadingSequences) {
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

    void renderMySQLDatabaseNode(MySQLDatabase* mysqlDb, MySQLDatabaseNode* dbData) {
        if (!mysqlDb || !dbData) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        const std::string nodeId =
            std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
        const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                                   ImGui::GetColorU32(colors.blue));

        // Handle expand/collapse
        if (ImGui::IsItemToggledOpen()) {
            dbData->expanded = isOpen;
        }

        if (isOpen) {
            // MySQL: render tables and views directly (no schema layer)

            // Render Tables section
            {
                const std::string tablesNodeId = std::format("tables_{}_{:p}", dbData->name,
                                                             static_cast<void*>(&dbData->tables));
                const bool tablesOpen = renderTreeNodeWithIcon(
                    "Tables", tablesNodeId, ICON_FK_TABLE, ImGui::GetColorU32(colors.green));

                if (tablesOpen) {
                    if (!dbData->tablesLoaded && !dbData->loadingTables) {
                        dbData->startTablesLoadAsync();
                    }

                    if (dbData->loadingTables) {
                        dbData->checkTablesStatusAsync();
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading tables...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_tables", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
                        ImGui::PopStyleColor();
                    } else if (dbData->tablesLoaded) {
                        if (dbData->tables.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No tables");
                            ImGui::PopStyleColor();
                        } else {
                            for (auto& table : dbData->tables) {
                                renderMySQLTableNode(table, dbData, mysqlDb);
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

                if (viewsOpen) {
                    if (!dbData->viewsLoaded && !dbData->loadingViews) {
                        dbData->startViewsLoadAsync();
                    }

                    if (dbData->loadingViews) {
                        dbData->checkViewsStatusAsync();
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading views...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_views", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
                        ImGui::PopStyleColor();
                    } else if (dbData->viewsLoaded) {
                        if (dbData->views.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No views");
                            ImGui::PopStyleColor();
                        } else {
                            for (auto& view : dbData->views) {
                                renderMySQLViewNode(view, dbData, mysqlDb);
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }

            ImGui::TreePop();
        }
    }

    void renderSQLiteDatabaseNode(SQLiteDatabase* sqliteDb, SQLiteDatabase::DatabaseData* dbData) {
        // TODO: Implement SQLite database node rendering using nested DatabaseData
    }

    void renderTableNode(Table& table, PostgresSchemaNode* schemaData,
                         const std::string& databaseName, const std::string& schemaName) {
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
            app.getTabManager()->createTableViewerTab(schemaData, table.name, databaseName,
                                                      schemaName);
        }

        // Context menu
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(schemaData, table.name, databaseName,
                                                          schemaName);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show table structure in a tab
            }
            ImGui::EndPopup();
        }

        if (tableOpen) {
            // Columns section
            {
                const std::string columnsNodeId =
                    std::format("columns_{}_{:p}", table.name, static_cast<void*>(&table.columns));
                const bool columnsOpen =
                    renderTreeNodeWithIcon("Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS,
                                           ImGui::GetColorU32(colors.green));

                if (columnsOpen) {
                    for (const auto& column : table.columns) {
                        ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                         ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                         ImGuiTreeNodeFlags_FramePadding;

                        std::string columnDisplay =
                            std::format("{} ({})", column.name, column.type);
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

            // Keys section
            {
                const std::string keysNodeId =
                    std::format("keys_{}_{:p}", table.name, static_cast<void*>(&table));
                const bool keysOpen = renderTreeNodeWithIcon("Keys", keysNodeId, ICON_FA_KEY,
                                                             ImGui::GetColorU32(colors.yellow));

                if (keysOpen) {
                    // Primary key
                    bool hasPrimaryKey = false;
                    std::string primaryKeyColumns;
                    for (const auto& column : table.columns) {
                        if (column.isPrimaryKey) {
                            if (hasPrimaryKey) {
                                primaryKeyColumns += ", ";
                            }
                            primaryKeyColumns += column.name;
                            hasPrimaryKey = true;
                        }
                    }

                    if (hasPrimaryKey) {
                        ImGuiTreeNodeFlags pkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string pkDisplay = "   Primary Key (" + primaryKeyColumns + ")";
                        ImGui::TreeNodeEx(pkDisplay.c_str(), pkFlags);
                    }

                    // Foreign keys
                    if (!table.foreignKeys.empty()) {
                        for (const auto& fk : table.foreignKeys) {
                            ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                         ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                         ImGuiTreeNodeFlags_FramePadding;
                            std::string fkDisplay =
                                std::format("   Foreign Key: {} -> {}.{}", fk.sourceColumn,
                                            fk.targetTable, fk.targetColumn);
                            ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);
                        }
                    }

                    if (!hasPrimaryKey && table.foreignKeys.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No keys defined");
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
                const std::string referencesNodeId =
                    std::format("references_{}_{:p}", table.name,
                                static_cast<void*>(&table.incomingForeignKeys));
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
                                std::format("{}.{} → {}.{}", ref.targetTable, ref.sourceColumn,
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

    void renderViewNode(Table& view, PostgresSchemaNode* schemaData,
                        const std::string& databaseName, const std::string& schemaName) {
        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
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
            auto& app = Application::getInstance();
            app.getTabManager()->createTableViewerTab(schemaData, view.name, databaseName,
                                                      schemaName);
        }

        // Context menu
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createTableViewerTab(schemaData, view.name, databaseName,
                                                          schemaName);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
    }

    void renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData, MySQLDatabase* mysqlDb) {
        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_Leaf |
                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                        ImGuiTreeNodeFlags_FramePadding;

        const std::string tableNodeId =
            std::format("table_{}_{:p}", table.name, static_cast<void*>(&table));
        const std::string tableLabel = std::format("   {}###{}", table.name, tableNodeId);
        ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

        // Draw icon
        const auto iconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.green),
                                            ICON_FK_TABLE);

        // Double-click to open table viewer
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            app.getTabManager()->createTableViewerTab(dbData, table.name, mysqlDb);
        }

        // Context menu
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(dbData, table.name, mysqlDb);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show table structure in a tab
            }
            ImGui::EndPopup();
        }
    }

    void renderMySQLViewNode(Table& view, MySQLDatabaseNode* dbData, MySQLDatabase* mysqlDb) {
        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
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
            app.getTabManager()->createTableViewerTab(dbData, view.name, mysqlDb);
        }

        // Context menu
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(dbData, view.name, mysqlDb);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
    }

} // namespace NewHierarchy
