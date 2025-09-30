#include "ui/hierarchy_helpers.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "ui/drop_column_dialog.hpp"
#include "ui/log_panel.hpp"
#include "ui/table_dialog.hpp"
#include "utils/spinner.hpp"

namespace HierarchyHelpers {
    // Forward declarations for external dialogs
    extern TableDialog& getTableDialog();
    extern DropColumnDialog& getDropColumnDialog();

    // Helper functions to reduce code duplication
    namespace {
        // Renders an icon at the tree node position
        void renderTreeNodeIcon(const char* icon, const ImVec4& color) {
            const auto iconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(color), icon);
        }

        // Creates a tree node label with icon spacing and unique ID
        std::string makeTreeNodeLabel(const std::string& text, const std::string& id = "") {
            if (id.empty()) {
                return std::format("   {}", text);
            }
            return std::format("   {}###{}", text, id);
        }

        // Attempts to switch database if needed (returns true if successful or not needed)
        bool ensureDatabaseSwitch(const std::shared_ptr<DatabaseInterface>& db,
                                  const std::string& targetDbName) {
            if (db->getType() == DatabaseType::POSTGRESQL) {
                const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (pgDb && targetDbName != pgDb->getDatabaseName()) {
                    LogPanel::debug("Auto-switching to database: " + targetDbName);
                    auto [success, error] = pgDb->switchToDatabase(targetDbName);
                    if (!success) {
                        LogPanel::error("Failed to switch database: " + error);
                        return false;
                    }
                }
            } else if (db->getType() == DatabaseType::MYSQL) {
                const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
                if (mysqlDb && targetDbName != mysqlDb->getDatabaseName()) {
                    LogPanel::debug("Auto-switching to database: " + targetDbName);
                    auto [success, error] = mysqlDb->switchToDatabase(targetDbName);
                    if (!success) {
                        LogPanel::error("Failed to switch database: " + error);
                        return false;
                    }
                }
            }
            return true;
        }

        // Gets schema name for PostgreSQL (returns empty for other DB types)
        std::string getSchemaName(const std::shared_ptr<DatabaseInterface>& db) {
            return (db->getType() == DatabaseType::POSTGRESQL) ? "public" : "";
        }

        // Renders loading state with spinner
        void renderLoadingState(const char* message, const char* spinnerId) {
            ImGui::Text("  %s", message);
            ImGui::SameLine();
            UIUtils::Spinner(spinnerId, 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Renders column node with context menu
        void renderColumnNode(const std::shared_ptr<DatabaseInterface>& db,
                              const std::string& tableName, const Column& column,
                              const std::string& schemaName = "") {
            const auto& [name, type, comment, isPrimaryKey, isNotNull] = column;
            ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

            std::string columnDisplay = std::format("{} ({})", name, type);
            if (isPrimaryKey) {
                columnDisplay += ", PK";
            }
            if (isNotNull) {
                columnDisplay += ", NOT NULL";
            }
            columnDisplay += ")";

            ImGui::PushID(name.c_str());
            ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);

            if (ImGui::BeginPopupContextItem("column_context_menu")) {
                if (ImGui::MenuItem("Edit Table")) {
                    std::string schema = schemaName.empty() ? getSchemaName(db) : schemaName;
                    getTableDialog().showTableDialog(db, tableName, schema);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Drop Column")) {
                    getDropColumnDialog().showDropColumnDialog(db, tableName, name);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    } // anonymous namespace

    void renderTableNode(const std::shared_ptr<DatabaseInterface>& db, int tableIndex,
                         const std::string& schemaName) {
        auto& app = Application::getInstance();
        auto& table = db->getTables()[tableIndex];

        ImGuiTreeNodeFlags tableFlags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

        if (table.expanded) {
            tableFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string tableLabel =
            makeTreeNodeLabel(table.name, std::format("table_{}_{}", db->getName(), table.name));
        const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);
        table.expanded = tableOpened;

        renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

        // Double-click to open table viewer (async loading will be handled by the tab)
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            app.getTabManager()->createTableViewerTab(db, table.name);
        }

        // Context menu
        ImGui::PushID(tableIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(db, table.name);
            }
            if (ImGui::MenuItem("Edit Table")) {
                std::string schema = schemaName.empty() ? getSchemaName(db) : schemaName;
                getTableDialog().showTableDialog(db, table.name, schema);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show table structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();

        if (tableOpened) {
            // Columns section
            constexpr ImGuiTreeNodeFlags columnsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                        ImGuiTreeNodeFlags_FramePadding;
            const std::string columnsLabel = makeTreeNodeLabel("Columns");
            const bool columnsOpened = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);

            renderTreeNodeIcon(ICON_FA_TABLE_COLUMNS, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));

            // Context menu for Columns section
            if (ImGui::BeginPopupContextItem("columns_context_menu")) {
                if (ImGui::MenuItem("Edit Table")) {
                    std::string schema = schemaName.empty() ? getSchemaName(db) : schemaName;
                    getTableDialog().showTableDialog(db, table.name, schema);
                }
                ImGui::EndPopup();
            }

            if (columnsOpened) {
                for (const auto& column : table.columns) {
                    renderColumnNode(db, table.name, column, schemaName);
                }
                ImGui::TreePop();
            }

            // Keys section
            constexpr ImGuiTreeNodeFlags keysFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                     ImGuiTreeNodeFlags_FramePadding;
            const std::string keysLabel = makeTreeNodeLabel("Keys");
            bool keysOpen = ImGui::TreeNodeEx(keysLabel.c_str(), keysFlags);

            renderTreeNodeIcon(ICON_FA_KEY, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));

            if (keysOpen) {
                // Show primary key if any column is marked as primary key
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
                    constexpr ImGuiTreeNodeFlags pkFlags = ImGuiTreeNodeFlags_Leaf |
                                                           ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                           ImGuiTreeNodeFlags_FramePadding;
                    std::string pkDisplay = "Primary Key (" + primaryKeyColumns + ")";
                    ImGui::TreeNodeEx(pkDisplay.c_str(), pkFlags);
                }

                // Show foreign keys
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        constexpr ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                               ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                               ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay =
                            std::format("Foreign Key: {} -> {}.{}", fk.sourceColumn, fk.targetTable,
                                        fk.targetColumn);
                        if (!fk.name.empty()) {
                            fkDisplay = std::format("{}: {}", fkDisplay, fk.name);
                        }
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);
                    }
                }

                if (!hasPrimaryKey && table.foreignKeys.empty()) {
                    ImGui::Text("  No keys defined");
                }
                ImGui::TreePop();
            }

            // Indexes section
            constexpr ImGuiTreeNodeFlags indexesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                        ImGuiTreeNodeFlags_FramePadding;
            const std::string indexesLabel = makeTreeNodeLabel("Indexes");
            bool indexesOpen = ImGui::TreeNodeEx(indexesLabel.c_str(), indexesFlags);

            renderTreeNodeIcon(ICON_FA_MAGNIFYING_GLASS, ImVec4(0.7f, 0.7f, 0.9f, 1.0f));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        constexpr ImGuiTreeNodeFlags indexFlags =
                            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
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
                        if (!index.type.empty() && index.type != "BTREE") {
                            indexDisplay += " " + index.type;
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::Text("  No indexes defined");
                }
                ImGui::TreePop();
            }

            // References section (tables that reference this table)
            if (!table.incomingForeignKeys.empty()) {
                constexpr ImGuiTreeNodeFlags referencesFlags =
                    ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                    ImGuiTreeNodeFlags_FramePadding;
                const std::string referencesLabel = makeTreeNodeLabel("References");
                bool referencesOpen = ImGui::TreeNodeEx(referencesLabel.c_str(), referencesFlags);

                renderTreeNodeIcon(ICON_FA_ARROW_RIGHT_TO_BRACKET, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));

                if (referencesOpen) {
                    for (const auto& ref : table.incomingForeignKeys) {
                        constexpr ImGuiTreeNodeFlags refFlags =
                            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                            ImGuiTreeNodeFlags_FramePadding;

                        // Format: referencing_table.referencing_column
                        // (targetTable contains the referencing table, sourceColumn is the column
                        // in that table)
                        std::string refDisplay =
                            std::format("{}.{}",
                                        ref.targetTable,   // The table that references this one
                                        ref.sourceColumn); // The column in that table
                        ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                        // Show full reference details on hover
                        if (ImGui::IsItemHovered()) {
                            std::string tooltip =
                                std::format("{}.{} → {}.{}", ref.targetTable, ref.sourceColumn,
                                            table.name, ref.targetColumn);
                            if (!ref.name.empty()) {
                                tooltip += std::format("\nConstraint: {}", ref.name);
                            }
                            if (!ref.onDelete.empty()) {
                                tooltip += std::format("\nON DELETE: {}", ref.onDelete);
                            }
                            if (!ref.onUpdate.empty()) {
                                tooltip += std::format("\nON UPDATE: {}", ref.onUpdate);
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

    void renderViewNode(const std::shared_ptr<DatabaseInterface>& db, const int viewIndex) {
        const auto& app = Application::getInstance();
        const auto& view = db->getViews()[viewIndex];

        constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

        const std::string viewLabel =
            makeTreeNodeLabel(view.name, std::format("view_{}_{}", db->getName(), view.name));
        ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

        renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

        const auto& tabManager = app.getTabManager();
        // Double-click to open view viewer (async loading will be handled by the tab)
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            tabManager->createTableViewerTab(db, view.name);
        }

        // Context menu
        ImGui::PushID(viewIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                tabManager->createTableViewerTab(db, view.name);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void renderTablesSection(const std::shared_ptr<DatabaseInterface>& db,
                             const std::string& schemaName) {
        // Get expansion state from the current database data
        bool tablesExpanded = false;
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            tablesExpanded = pgDb->getCurrentDatabaseData().tablesExpanded;
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            tablesExpanded = mysqlDb->getCurrentDatabaseData().tablesExpanded;
        }

        ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        // Set the default open state based on the expansion state
        if (tablesExpanded) {
            tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const bool showTablesSpinner = db->isLoadingTables();

        const std::string tablesLabel =
            makeTreeNodeLabel(std::format("Tables ({})", db->getTables().size()),
                              std::format("tables_current_{}", db->getName()));
        const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

        // Update the expansion state based on the current UI state
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            pgDb->getCurrentDatabaseData().tablesExpanded = tablesOpen;
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            mysqlDb->getCurrentDatabaseData().tablesExpanded = tablesOpen;
        }

        renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

        if (showTablesSpinner) {
            ImGui::SameLine();
            UIUtils::Spinner("##tables_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Context menu for Tables section
        if (ImGui::BeginPopupContextItem("tables_context_menu")) {
            if (ImGui::MenuItem("Create New Table")) {
                std::string schema = schemaName.empty() ? getSchemaName(db) : schemaName;
                getTableDialog().showCreateTableDialog(db, schema);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh")) {
                db->setTablesLoaded(false);
                // For PostgreSQL, pass the schema name
                if (db->getType() == DatabaseType::POSTGRESQL && !schemaName.empty()) {
                    auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                    pgDb->refreshTables(schemaName);
                } else {
                    db->refreshTables();
                }
            }
            ImGui::EndPopup();
        }

        // Load tables when the tree node is opened and tables haven't been loaded yet
        if (tablesOpen && !db->areTablesLoaded() && !db->isLoadingTables()) {
            LogPanel::debug(
                "Tables node expanded and tables not loaded yet, attempting to load...");
            // For PostgreSQL, pass the schema name
            if (db->getType() == DatabaseType::POSTGRESQL && !schemaName.empty()) {
                auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                pgDb->refreshTables(schemaName);
            } else {
                db->refreshTables();
            }
        }

        if (tablesOpen) {
            if (db->getTables().empty()) {
                if (db->isLoadingTables()) {
                    renderLoadingState("Loading tables...", "##loading_tables_spinner");
                } else if (!db->areTablesLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No tables found");
                }
            } else {
                for (int j = 0; j < db->getTables().size(); j++) {
                    renderTableNode(db, j, schemaName);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderViewsSection(const std::shared_ptr<DatabaseInterface>& db,
                            const std::string& schemaName) {
        // Get expansion state from the current database data
        bool viewsExpanded = false;
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            viewsExpanded = pgDb->getCurrentDatabaseData().viewsExpanded;
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            viewsExpanded = mysqlDb->getCurrentDatabaseData().viewsExpanded;
        }

        ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                        ImGuiTreeNodeFlags_FramePadding;

        // Set the default open state based on the expansion state
        if (viewsExpanded) {
            viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const bool showViewsSpinner = db->isLoadingViews();

        const std::string viewsLabel =
            makeTreeNodeLabel(std::format("Views ({})", db->getViews().size()),
                              std::format("views_current_{}", db->getName()));
        const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

        // Update the expansion state based on the current UI state
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            pgDb->getCurrentDatabaseData().viewsExpanded = viewsOpen;
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            mysqlDb->getCurrentDatabaseData().viewsExpanded = viewsOpen;
        }

        renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

        if (showViewsSpinner) {
            ImGui::SameLine();
            UIUtils::Spinner("##views_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load views when the tree node is opened and views haven't been loaded yet
        if (viewsOpen && !db->areViewsLoaded() && !db->isLoadingViews()) {
            LogPanel::debug("Views node expanded and views not loaded yet, attempting to load...");
            // For PostgreSQL, pass the schema name
            if (db->getType() == DatabaseType::POSTGRESQL && !schemaName.empty()) {
                auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                pgDb->refreshViews(schemaName);
            } else {
                db->refreshViews();
            }
        }

        if (viewsOpen) {
            if (db->getViews().empty()) {
                if (db->isLoadingViews()) {
                    renderLoadingState("Loading views...", "##loading_views_spinner");
                } else if (!db->areViewsLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No views found");
                }
            } else {
                for (int j = 0; j < db->getViews().size(); j++) {
                    renderViewNode(db, j);
                }
            }
            ImGui::TreePop();
        }
    }
    void renderRedisHierarchy(const std::shared_ptr<DatabaseInterface>& db) {
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

        // Show Redis connection info
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), ICON_FA_DATABASE " Connected");

        // Load keys if not loaded yet
        if (!redisDb->areTablesLoaded() && !redisDb->isLoadingTables()) {
            redisDb->refreshTables();
        }

        // Show loading indicator if loading
        if (redisDb->isLoadingTables()) {
            renderLoadingState("Loading keys...", "##loading_redis_keys_spinner");
            return;
        }

        // Show key groups directly (no nested Keys section)
        if (redisDb->getTables().empty()) {
            if (!redisDb->areTablesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No keys found");
            }
        } else {
            const auto& tables = redisDb->getTables();
            for (const auto& table : tables) {
                constexpr ImGuiTreeNodeFlags keyGroupFlags = ImGuiTreeNodeFlags_Leaf |
                                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                             ImGuiTreeNodeFlags_FramePadding;

                const std::string displayName = (table.name == "*") ? "All Keys" : table.name;
                const std::string keyGroupLabel = makeTreeNodeLabel(displayName);
                ImGui::TreeNodeEx(keyGroupLabel.c_str(), keyGroupFlags);

                renderTreeNodeIcon(ICON_FA_KEY, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));

                // Double-click to open Redis key viewer
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createTableViewerTab(redisDb, table.name);
                }

                // Context menu
                ImGui::PushID(table.name.c_str());
                if (ImGui::BeginPopupContextItem(nullptr)) {
                    if (ImGui::MenuItem("View Keys")) {
                        auto& app = Application::getInstance();
                        app.getTabManager()->createTableViewerTab(redisDb, table.name);
                    }
                    if (ImGui::MenuItem("Refresh Keys")) {
                        redisDb->setTablesLoaded(false);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }

        // Add Query node below All Keys
        constexpr ImGuiTreeNodeFlags queryFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;

        const std::string queryLabel = makeTreeNodeLabel("Query");
        ImGui::TreeNodeEx(queryLabel.c_str(), queryFlags);

        renderTreeNodeIcon(ICON_FA_TERMINAL, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));

        // Double-click to open Redis query editor
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            const auto& app = Application::getInstance();
            app.getTabManager()->createSQLEditorTab("", db);
        }

        // Context menu for Query
        if (ImGui::BeginPopupContextItem("query_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", db);
            }
            ImGui::EndPopup();
        }

        // Redis-specific info section
        constexpr ImGuiTreeNodeFlags infoFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                 ImGuiTreeNodeFlags_FramePadding;

        const std::string infoLabel = makeTreeNodeLabel("Server Info", "redis_info");
        const bool infoOpen = ImGui::TreeNodeEx(infoLabel.c_str(), infoFlags);

        renderTreeNodeIcon(ICON_FA_CIRCLE_INFO, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));

        if (infoOpen) {
            ImGui::Text("  Host: %s", redisDb->getConnectionString().c_str());
            ImGui::Text("  Type: Redis Key-Value Store");

            // Add a button to refresh keys
            if (ImGui::Button("  " ICON_FA_ARROWS_ROTATE " Refresh Keys")) {
                redisDb->setTablesLoaded(false);
            }

            ImGui::TreePop();
        }
    }

    void renderCachedTablesSection(const std::shared_ptr<DatabaseInterface>& db,
                                   const std::string& dbName) {
        // For PostgreSQL and MySQL, we need to cast to access cached data
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            const auto& dbData = pgDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.tablesExpanded) {
                tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string tablesLabel =
                makeTreeNodeLabel(std::format("Tables ({})", dbData.tables.size()),
                                  std::format("tables_cached_pg_{}", dbName));
            const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

            pgDb->getDatabaseData(dbName).tablesExpanded = tablesOpen;

            renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

            if (dbData.loadingTables) {
                ImGui::SameLine();
                UIUtils::Spinner(("##tables_spinner_" + dbName).c_str(), 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            }

            // Context menu for cached Tables section (PostgreSQL)
            if (ImGui::BeginPopupContextItem(("cached_tables_context_menu_pg_" + dbName).c_str())) {
                if (ImGui::MenuItem("Create New Table")) {
                    getTableDialog().showCreateTableDialog(db, getSchemaName(db));
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Refresh")) {
                    // Auto-switch to the correct database before refreshing
                    if (dbName != pgDb->getDatabaseName()) {
                        if (!pgDb->isSwitchingDatabase()) {
                            LogPanel::debug("Auto-switching to database: " + dbName +
                                            " to refresh tables");
                            pgDb->switchToDatabaseAsync(dbName);
                        }
                    } else {
                        pgDb->setTablesLoaded(false);
                        pgDb->refreshTables();
                    }
                }
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (dbData.tables.empty()) {
                    if (dbData.loadingTables) {
                        ImGui::Text("  Loading tables...");
                    } else if (!dbData.tablesLoaded) {
                        // Auto-switch database and load tables when node is expanded
                        if (dbName != pgDb->getDatabaseName()) {
                            if (!pgDb->isSwitchingDatabase()) {
                                LogPanel::debug("Auto-switching to database: " + dbName +
                                                " to load tables");
                                pgDb->switchToDatabaseAsync(dbName);
                            }
                            ImGui::Text("  Switching database...");
                        } else {
                            pgDb->refreshTables();
                        }
                        ImGui::Text("  Loading tables...");
                    } else {
                        ImGui::Text("  No tables found");
                    }
                } else {
                    for (int j = 0; j < dbData.tables.size(); j++) {
                        renderCachedTableNode(db, dbName, j);
                    }
                }
                ImGui::TreePop();
            }
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            auto& dbData = mysqlDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.tablesExpanded) {
                tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string tablesLabel =
                makeTreeNodeLabel(std::format("Tables ({})", dbData.tables.size()),
                                  std::format("tables_cached_mysql_{}", dbName));
            const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

            dbData.tablesExpanded = tablesOpen;

            renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

            if (dbData.loadingTables) {
                ImGui::SameLine();
                UIUtils::Spinner(("##tables_spinner_" + dbName).c_str(), 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            }

            // Context menu for cached Tables section (MySQL)
            if (ImGui::BeginPopupContextItem(
                    ("cached_tables_context_menu_mysql_" + dbName).c_str())) {
                if (ImGui::MenuItem("Create New Table")) {
                    getTableDialog().showCreateTableDialog(db, "");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Refresh")) {
                    // Auto-switch to the correct database before refreshing
                    if (dbName != mysqlDb->getDatabaseName()) {
                        if (!mysqlDb->isSwitchingDatabase()) {
                            LogPanel::debug("Auto-switching to database: " + dbName +
                                            " to refresh tables");
                            mysqlDb->switchToDatabaseAsync(dbName);
                        }
                    } else {
                        mysqlDb->setTablesLoaded(false);
                        mysqlDb->refreshTables();
                    }
                }
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (dbData.tables.empty()) {
                    if (dbData.loadingTables) {
                        ImGui::Text("  Loading tables...");
                    } else if (!dbData.tablesLoaded) {
                        // Auto-switch database and load tables when node is expanded
                        if (dbName != mysqlDb->getDatabaseName()) {
                            if (!mysqlDb->isSwitchingDatabase()) {
                                LogPanel::debug("Auto-switching to database: " + dbName +
                                                " to load tables");
                                mysqlDb->switchToDatabaseAsync(dbName);
                            }
                            ImGui::Text("  Switching database...");
                        } else {
                            mysqlDb->refreshTables();
                        }
                        ImGui::Text("  Loading tables...");
                    } else {
                        ImGui::Text("  No tables found");
                    }
                } else {
                    for (int j = 0; j < dbData.tables.size(); j++) {
                        renderCachedTableNode(db, dbName, j);
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    void renderCachedViewsSection(const std::shared_ptr<DatabaseInterface>& db,
                                  const std::string& dbName) {
        // For PostgreSQL and MySQL, we need to cast to access cached data
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            auto& dbData = pgDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.viewsExpanded) {
                viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string viewsLabel =
                makeTreeNodeLabel(std::format("Views ({})", dbData.views.size()),
                                  std::format("views_cached_pg_{}", dbName));
            const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

            dbData.viewsExpanded = viewsOpen;

            renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

            if (dbData.loadingViews) {
                ImGui::SameLine();
                UIUtils::Spinner(("##views_spinner_" + dbName).c_str(), 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            }

            if (viewsOpen) {
                if (dbData.views.empty()) {
                    if (dbData.loadingViews) {
                        ImGui::Text("  Loading views...");
                    } else if (!dbData.viewsLoaded) {
                        // Auto-switch database and load views when node is expanded
                        if (dbName != pgDb->getDatabaseName()) {
                            if (!pgDb->isSwitchingDatabase()) {
                                LogPanel::debug("Auto-switching to database: " + dbName +
                                                " to load views");
                                pgDb->switchToDatabaseAsync(dbName);
                            }
                            ImGui::Text("  Switching database...");
                        } else {
                            pgDb->refreshViews();
                        }
                        ImGui::Text("  Loading views...");
                    } else {
                        ImGui::Text("  No views found");
                    }
                } else {
                    for (int j = 0; j < dbData.views.size(); j++) {
                        renderCachedViewNode(db, dbName, j);
                    }
                }
                ImGui::TreePop();
            }
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            auto& dbData = mysqlDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.viewsExpanded) {
                viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string viewsLabel =
                makeTreeNodeLabel(std::format("Views ({})", dbData.views.size()),
                                  std::format("views_cached_mysql_{}", dbName));
            const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

            dbData.viewsExpanded = viewsOpen;

            renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

            if (dbData.loadingViews) {
                ImGui::SameLine();
                UIUtils::Spinner(("##views_spinner_" + dbName).c_str(), 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            }

            if (viewsOpen) {
                if (dbData.views.empty()) {
                    if (dbData.loadingViews) {
                        ImGui::Text("  Loading views...");
                    } else if (!dbData.viewsLoaded) {
                        // Auto-switch database and load views when node is expanded
                        if (dbName != mysqlDb->getDatabaseName()) {
                            if (!mysqlDb->isSwitchingDatabase()) {
                                LogPanel::debug("Auto-switching to database: " + dbName +
                                                " to load views");
                                mysqlDb->switchToDatabaseAsync(dbName);
                            }
                            ImGui::Text("  Switching database...");
                        } else {
                            mysqlDb->refreshViews();
                        }
                        ImGui::Text("  Loading views...");
                    } else {
                        ImGui::Text("  No views found");
                    }
                } else {
                    for (int j = 0; j < dbData.views.size(); j++) {
                        renderCachedViewNode(db, dbName, j);
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    void renderCachedTableNode(const std::shared_ptr<DatabaseInterface>& db,
                               const std::string& dbName, int tableIndex) {
        auto& app = Application::getInstance();

        // Get the cached table data
        Table* table = nullptr;
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            auto& dbData = pgDb->getDatabaseData(dbName);
            if (tableIndex < dbData.tables.size()) {
                table = &dbData.tables[tableIndex];
            }
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            auto& dbData = mysqlDb->getDatabaseData(dbName);
            if (tableIndex < dbData.tables.size()) {
                table = &dbData.tables[tableIndex];
            }
        }

        if (!table)
            return;

        ImGuiTreeNodeFlags tableFlags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

        // Set the default open state based on the table's expanded state
        if (table->expanded) {
            tableFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string tableLabel =
            makeTreeNodeLabel(table->name, std::format("cached_table_{}_{}", dbName, table->name));
        const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

        table->expanded = tableOpened;

        renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

        // Double-click to open table viewer
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (!ensureDatabaseSwitch(db, dbName)) {
                return;
            }
            app.getTabManager()->createTableViewerTab(db, table->name);
        }

        // Context menu
        ImGui::PushID(tableIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                if (!ensureDatabaseSwitch(db, dbName)) {
                    ImGui::EndPopup();
                    ImGui::PopID();
                    return;
                }
                app.getTabManager()->createTableViewerTab(db, table->name);
            }
            if (ImGui::MenuItem("Edit Table")) {
                getTableDialog().showTableDialog(db, table->name, getSchemaName(db));
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show table structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();

        if (tableOpened) {
            // Show cached columns
            constexpr ImGuiTreeNodeFlags columnsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                        ImGuiTreeNodeFlags_FramePadding;
            const std::string columnsLabel = makeTreeNodeLabel("Columns");
            const bool columnsOpened = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);

            renderTreeNodeIcon(ICON_FA_TABLE_COLUMNS, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));

            // Context menu for Columns section
            if (ImGui::BeginPopupContextItem("cached_columns_context_menu")) {
                if (ImGui::MenuItem("Edit Table")) {
                    getTableDialog().showTableDialog(db, table->name, getSchemaName(db));
                }
                ImGui::EndPopup();
            }

            if (columnsOpened) {
                for (const auto& column : table->columns) {
                    renderColumnNode(db, table->name, column);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }

    void renderCachedViewNode(const std::shared_ptr<DatabaseInterface>& db,
                              const std::string& dbName, int viewIndex) {
        auto& app = Application::getInstance();

        // Get the cached view data
        const Table* view = nullptr;
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            const auto& dbData = pgDb->getDatabaseData(dbName);
            if (viewIndex < dbData.views.size()) {
                view = &dbData.views[viewIndex];
            }
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            const auto& dbData = mysqlDb->getDatabaseData(dbName);
            if (viewIndex < dbData.views.size()) {
                view = &dbData.views[viewIndex];
            }
        }

        if (!view)
            return;

        constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

        const std::string viewLabel =
            makeTreeNodeLabel(view->name, std::format("cached_view_{}_{}", dbName, view->name));
        ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

        renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

        const auto& tabManager = app.getTabManager();
        // Double-click to open view viewer
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (!ensureDatabaseSwitch(db, dbName)) {
                return;
            }
            tabManager->createTableViewerTab(db, view->name);
        }

        // Context menu
        ImGui::PushID(viewIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                if (!ensureDatabaseSwitch(db, dbName)) {
                    ImGui::EndPopup();
                    ImGui::PopID();
                    return;
                }
                tabManager->createTableViewerTab(db, view->name);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
} // namespace HierarchyHelpers
