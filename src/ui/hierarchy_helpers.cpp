#include "ui/hierarchy_helpers.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "ui/column_dialog.hpp"
#include "ui/log_panel.hpp"
#include "utils/spinner.hpp"

namespace HierarchyHelpers {
    // Forward declaration for external column dialog
    extern ColumnDialog &getColumnDialog();

    void renderTableNode(const std::shared_ptr<DatabaseInterface> &db, int tableIndex) {
        auto &app = Application::getInstance();
        auto &table = db->getTables()[tableIndex];

        ImGuiTreeNodeFlags tableFlags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

        // Set the default open state based on the table's expanded state
        if (table.expanded) {
            tableFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        // Draw tree node with placeholder space for icon
        const std::string tableLabel = std::format("   {}###table_{}_{}", table.name, db->getName(),
                                                   table.name); // 3 spaces for icon with unique ID
        const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

        // Update the table's expanded state based on the current UI state
        table.expanded = tableOpened;

        // Draw colored icon over the placeholder space
        const auto tableIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            tableIconPos, ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f)), // Green for tables
            ICON_FA_TABLE);

        // Double-click to open table viewer (async loading will be handled by the tab)
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
        }

        // Context menu
        ImGui::PushID(tableIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
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
            // Draw tree node with placeholder space for icon
            const std::string columnsLabel = "   Columns"; // 3 spaces for icon
            const bool columnsOpened = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);

            // Draw colored icon over the placeholder space
            const auto columnsIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(
                columnsIconPos, ImGui::GetColorU32(ImVec4(0.5f, 0.9f, 0.5f, 1.0f)), // Light green
                ICON_FA_TABLE_COLUMNS);

            // Context menu for Columns section
            if (ImGui::BeginPopupContextItem("columns_context_menu")) {
                if (ImGui::MenuItem("New Column")) {
                    getColumnDialog().showAddColumnDialog(db, table.name);
                }
                ImGui::EndPopup();
            }

            if (columnsOpened) {
                for (int colIndex = 0; colIndex < table.columns.size(); colIndex++) {
                    const auto &[name, type, isPrimaryKey, isNotNull] = table.columns[colIndex];
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    // Build column display string with type and constraints
                    std::string columnDisplay = std::format("{} ({})", name, type);
                    if (isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }
                    columnDisplay += ")";

                    ImGui::PushID(colIndex);
                    ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);

                    // Context menu for individual column
                    if (ImGui::BeginPopupContextItem("column_context_menu")) {
                        if (ImGui::MenuItem("Edit Column")) {
                            getColumnDialog().showEditColumnDialog(db, table.name,
                                                                   table.columns[colIndex]);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Drop Column")) {
                            // TODO: Implement drop column functionality
                            LogPanel::info("Drop column functionality not yet implemented");
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            // Keys section
            constexpr ImGuiTreeNodeFlags keysFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                     ImGuiTreeNodeFlags_FramePadding;
            // Draw tree node with placeholder space for icon
            const std::string keysLabel = "   Keys"; // 3 spaces for icon
            bool keysOpen = ImGui::TreeNodeEx(keysLabel.c_str(), keysFlags);

            // Draw colored icon over the placeholder space
            const auto keysIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(
                keysIconPos, ImGui::GetColorU32(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)), // Gold for Keys
                ICON_FA_KEY);

            if (keysOpen) {
                // Show primary key if any column is marked as primary key
                bool hasPrimaryKey = false;
                std::string primaryKeyColumns;
                for (const auto &column : table.columns) {
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
                } else {
                    ImGui::Text("  No primary key");
                }
                ImGui::TreePop();
            }

            // Indexes section
            constexpr ImGuiTreeNodeFlags indexesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                        ImGuiTreeNodeFlags_FramePadding;
            // Draw tree node with placeholder space for icon
            const std::string indexesLabel = "   Indexes"; // 3 spaces for icon
            bool indexesOpen = ImGui::TreeNodeEx(indexesLabel.c_str(), indexesFlags);

            // Draw colored icon over the placeholder space
            const ImVec2 indexesIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(
                indexesIconPos,
                ImGui::GetColorU32(ImVec4(0.7f, 0.7f, 0.9f, 1.0f)), // Light purple for Indexes
                ICON_FA_MAGNIFYING_GLASS);

            if (indexesOpen) {
                // TODO: Implement index retrieval from database
                ImGui::Text("  Index information not available");
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }
    }

    void renderViewNode(const std::shared_ptr<DatabaseInterface> &db, const int viewIndex) {
        const auto &app = Application::getInstance();
        const auto &view = db->getViews()[viewIndex];

        constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string viewLabel = std::format("   {}###view_{}_{}", view.name, db->getName(),
                                                  view.name); // 3 spaces for icon with unique ID
        ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

        // Draw colored icon over the placeholder space
        const auto viewIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            viewIconPos, ImGui::GetColorU32(ImVec4(0.9f, 0.6f, 0.2f, 1.0f)), // Orange for views
            ICON_FA_EYE);

        const auto &tabManager = app.getTabManager();
        // Double-click to open view viewer (async loading will be handled by the tab)
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            tabManager->createTableViewerTab(db->getConnectionString(), view.name);
        }

        // Context menu
        ImGui::PushID(viewIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                tabManager->createTableViewerTab(db->getConnectionString(), view.name);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void renderTablesSection(const std::shared_ptr<DatabaseInterface> &db) {
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

        // Show loading indicator next to Tables node if loading
        const bool showTablesSpinner = db->isLoadingTables();

        // Draw tree node with placeholder space for icon
        const std::string tablesLabel =
            std::format("   Tables ({})###tables_current_{}", db->getTables().size(),
                        db->getName()); // 3 spaces for icon, unique ID per database
        const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

        // Update the expansion state based on the current UI state
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            pgDb->getCurrentDatabaseData().tablesExpanded = tablesOpen;
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            mysqlDb->getCurrentDatabaseData().tablesExpanded = tablesOpen;
        }

        // Draw colored icon over the placeholder space
        const auto tablesSectionIconPos =
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

        // Context menu for Tables section
        if (ImGui::BeginPopupContextItem("tables_context_menu")) {
            if (ImGui::MenuItem("Refresh")) {
                db->setTablesLoaded(false);
                db->refreshTables();
            }
            ImGui::EndPopup();
        }

        // Load tables when the tree node is opened and tables haven't been loaded yet
        if (tablesOpen && !db->areTablesLoaded() && !db->isLoadingTables()) {
            LogPanel::debug(
                "Tables node expanded and tables not loaded yet, attempting to load...");
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
                for (int j = 0; j < db->getTables().size(); j++) {
                    renderTableNode(db, j);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderViewsSection(const std::shared_ptr<DatabaseInterface> &db) {
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

        // Show loading indicator next to Views node if loading
        const bool showViewsSpinner = db->isLoadingViews();

        // Draw tree node with placeholder space for icon
        const std::string viewsLabel =
            std::format("   Views ({})###views_current_{}", db->getViews().size(),
                        db->getName()); // 3 spaces for icon, unique ID per database
        const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

        // Update the expansion state based on the current UI state
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            pgDb->getCurrentDatabaseData().viewsExpanded = viewsOpen;
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            mysqlDb->getCurrentDatabaseData().viewsExpanded = viewsOpen;
        }

        // Draw colored icon over the placeholder space
        const auto viewsSectionIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            viewsSectionIconPos,
            ImGui::GetColorU32(ImVec4(0.9f, 0.6f, 0.2f, 1.0f)), // Orange for Views section
            ICON_FA_EYE);

        // Show spinner next to Views node if loading
        if (showViewsSpinner) {
            ImGui::SameLine();
            UIUtils::Spinner("##views_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load views when the tree node is opened and views haven't been loaded yet
        if (viewsOpen && !db->areViewsLoaded() && !db->isLoadingViews()) {
            LogPanel::debug("Views node expanded and views not loaded yet, attempting to load...");
            db->refreshViews();
        }

        if (viewsOpen) {
            if (db->getViews().empty()) {
                if (db->isLoadingViews()) {
                    // Show loading indicator with spinner
                    ImGui::Text("  Loading views...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_views_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
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
    void renderRedisHierarchy(const std::shared_ptr<DatabaseInterface> &db) {
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
            ImGui::Text("  Loading keys...");
            ImGui::SameLine();
            UIUtils::Spinner("##loading_redis_keys_spinner", 6.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
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
            const auto &tables = redisDb->getTables();
            for (int i = 0; i < tables.size(); i++) {
                const auto &table = tables[i];

                constexpr ImGuiTreeNodeFlags keyGroupFlags = ImGuiTreeNodeFlags_Leaf |
                                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                             ImGuiTreeNodeFlags_FramePadding;

                // Draw tree node with placeholder space for icon
                // Display user-friendly name but use the actual pattern for functionality
                const std::string displayName = (table.name == "*") ? "All Keys" : table.name;
                const std::string keyGroupLabel =
                    std::format("   {}", displayName); // 3 spaces for icon
                ImGui::TreeNodeEx(keyGroupLabel.c_str(), keyGroupFlags);

                // Draw colored icon over the placeholder space
                const ImVec2 keyGroupIconPos =
                    ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                           ImGui::GetItemRectMin().y +
                               (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

                ImGui::GetWindowDrawList()->AddText(
                    keyGroupIconPos,
                    ImGui::GetColorU32(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)), // Gold for Redis keys
                    ICON_FA_KEY);

                // Double-click to open Redis key viewer
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    auto &app = Application::getInstance();
                    app.getTabManager()->createTableViewerTab(redisDb->getConnectionString(),
                                                              table.name);
                }

                // Context menu
                ImGui::PushID(i);
                if (ImGui::BeginPopupContextItem(nullptr)) {
                    if (ImGui::MenuItem("View Keys")) {
                        auto &app = Application::getInstance();
                        app.getTabManager()->createTableViewerTab(redisDb->getConnectionString(),
                                                                  table.name);
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

        const std::string queryLabel = "   Query"; // 3 spaces for icon
        ImGui::TreeNodeEx(queryLabel.c_str(), queryFlags);

        // Draw colored icon over the placeholder space
        const auto queryIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            queryIconPos,
            ImGui::GetColorU32(ImVec4(0.4f, 0.8f, 1.0f, 1.0f)), // Light blue for query
            ICON_FA_TERMINAL);

        // Double-click to open Redis query editor
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            const auto &app = Application::getInstance();
            app.getTabManager()->createSQLEditorTab("", db);
        }

        // Context menu for Query
        if (ImGui::BeginPopupContextItem("query_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto &app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", db);
            }
            ImGui::EndPopup();
        }

        // Redis-specific info section
        constexpr ImGuiTreeNodeFlags infoFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                 ImGuiTreeNodeFlags_FramePadding;

        const std::string infoLabel = "   Server Info###redis_info"; // 3 spaces for icon
        const bool infoOpen = ImGui::TreeNodeEx(infoLabel.c_str(), infoFlags);

        // Draw colored icon over the placeholder space
        const auto infoIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            infoIconPos, ImGui::GetColorU32(ImVec4(0.4f, 0.8f, 1.0f, 1.0f)), // Light blue for info
            ICON_FA_CIRCLE_INFO);

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

    void renderCachedTablesSection(const std::shared_ptr<DatabaseInterface> &db,
                                   const std::string &dbName) {
        // For PostgreSQL and MySQL, we need to cast to access cached data
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            const auto &dbData = pgDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.tablesExpanded) {
                tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string tablesLabel =
                std::format("   Tables ({})###tables_cached_pg_{}", dbData.tables.size(), dbName);
            const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

            // Update the expansion state based on the current UI state
            pgDb->getDatabaseData(dbName).tablesExpanded = tablesOpen;

            const auto tablesSectionIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(tablesSectionIconPos,
                                                ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f)),
                                                ICON_FA_TABLE);

            if (dbData.loadingTables) {
                ImGui::SameLine();
                UIUtils::Spinner(("##tables_spinner_" + dbName).c_str(), 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
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
            auto &dbData = mysqlDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.tablesExpanded) {
                tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string tablesLabel = std::format("   Tables ({})###tables_cached_mysql_{}",
                                                        dbData.tables.size(), dbName);
            const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

            // Update the expansion state based on the current UI state
            dbData.tablesExpanded = tablesOpen;

            const auto tablesSectionIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(tablesSectionIconPos,
                                                ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f)),
                                                ICON_FA_TABLE);

            if (dbData.loadingTables) {
                ImGui::SameLine();
                UIUtils::Spinner(("##tables_spinner_" + dbName).c_str(), 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
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

    void renderCachedViewsSection(const std::shared_ptr<DatabaseInterface> &db,
                                  const std::string &dbName) {
        // For PostgreSQL and MySQL, we need to cast to access cached data
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            auto &dbData = pgDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.viewsExpanded) {
                viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string viewsLabel =
                std::format("   Views ({})###views_cached_pg_{}", dbData.views.size(), dbName);
            const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

            // Update the expansion state based on the current UI state
            dbData.viewsExpanded = viewsOpen;

            const auto viewsSectionIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(viewsSectionIconPos,
                                                ImGui::GetColorU32(ImVec4(0.9f, 0.6f, 0.2f, 1.0f)),
                                                ICON_FA_EYE);

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
            auto &dbData = mysqlDb->getDatabaseData(dbName);

            ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

            // Set the default open state based on the expansion state
            if (dbData.viewsExpanded) {
                viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string viewsLabel =
                std::format("   Views ({})###views_cached_mysql_{}", dbData.views.size(), dbName);
            const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

            // Update the expansion state based on the current UI state
            dbData.viewsExpanded = viewsOpen;

            const auto viewsSectionIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(viewsSectionIconPos,
                                                ImGui::GetColorU32(ImVec4(0.9f, 0.6f, 0.2f, 1.0f)),
                                                ICON_FA_EYE);

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

    void renderCachedTableNode(const std::shared_ptr<DatabaseInterface> &db,
                               const std::string &dbName, int tableIndex) {
        auto &app = Application::getInstance();

        // Get the cached table data
        Table *table = nullptr;
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            auto &dbData = pgDb->getDatabaseData(dbName);
            if (tableIndex < dbData.tables.size()) {
                table = &dbData.tables[tableIndex];
            }
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            auto &dbData = mysqlDb->getDatabaseData(dbName);
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

        // Draw tree node with placeholder space for icon
        const std::string tableLabel =
            std::format("   {}###cached_table_{}_{}", table->name, dbName,
                        table->name); // 3 spaces for icon with unique ID
        const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

        // Update the table's expanded state based on the current UI state
        table->expanded = tableOpened;

        // Draw colored icon over the placeholder space
        const ImVec2 tableIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            tableIconPos, ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f)), // Green for tables
            ICON_FA_TABLE);

        // Double-click to open table viewer
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            // Auto-switch to the correct database before opening table viewer
            if (db->getType() == DatabaseType::POSTGRESQL) {
                const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (pgDb && dbName != pgDb->getDatabaseName()) {
                    LogPanel::debug("Auto-switching to database: " + dbName +
                                    " to view table: " + table->name);
                    auto [success, error] = pgDb->switchToDatabase(dbName);
                    if (!success) {
                        LogPanel::error("Failed to switch database: " + error);
                        return;
                    }
                }
            } else if (db->getType() == DatabaseType::MYSQL) {
                const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
                if (mysqlDb && dbName != mysqlDb->getDatabaseName()) {
                    LogPanel::debug("Auto-switching to database: " + dbName +
                                    " to view table: " + table->name);
                    auto [success, error] = mysqlDb->switchToDatabase(dbName);
                    if (!success) {
                        LogPanel::error("Failed to switch database: " + error);
                        return;
                    }
                }
            }
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), table->name);
        }

        // Context menu
        ImGui::PushID(static_cast<int>(tableIndex));
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                // Auto-switch to the correct database before opening table viewer
                if (db->getType() == DatabaseType::POSTGRESQL) {
                    const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                    if (pgDb && dbName != pgDb->getDatabaseName()) {
                        LogPanel::debug("Auto-switching to database: " + dbName +
                                        " to view table: " + table->name);
                        auto [success, error] = pgDb->switchToDatabase(dbName);
                        if (!success) {
                            LogPanel::error("Failed to switch database: " + error);
                            ImGui::EndPopup();
                            ImGui::PopID();
                            return;
                        }
                    }
                } else if (db->getType() == DatabaseType::MYSQL) {
                    const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
                    if (mysqlDb && dbName != mysqlDb->getDatabaseName()) {
                        LogPanel::debug("Auto-switching to database: " + dbName +
                                        " to view table: " + table->name);
                        auto [success, error] = mysqlDb->switchToDatabase(dbName);
                        if (!success) {
                            LogPanel::error("Failed to switch database: " + error);
                            ImGui::EndPopup();
                            ImGui::PopID();
                            return;
                        }
                    }
                }
                app.getTabManager()->createTableViewerTab(db->getConnectionString(), table->name);
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
            const std::string columnsLabel = "   Columns"; // 3 spaces for icon
            const bool columnsOpened = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);

            const auto columnsIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(
                columnsIconPos,
                ImGui::GetColorU32(ImVec4(0.5f, 0.9f, 0.5f, 1.0f)), // Light green for Columns
                ICON_FA_TABLE_COLUMNS);

            // Context menu for Columns section
            if (ImGui::BeginPopupContextItem("cached_columns_context_menu")) {
                if (ImGui::MenuItem("New Column")) {
                    getColumnDialog().showAddColumnDialog(db, table->name);
                }
                ImGui::EndPopup();
            }

            if (columnsOpened) {
                for (int colIndex = 0; colIndex < table->columns.size(); colIndex++) {
                    const auto &[name, type, isPrimaryKey, isNotNull] = table->columns[colIndex];
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

                    ImGui::PushID(colIndex + 1000); // Offset to avoid ID conflicts
                    ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);

                    // Context menu for individual column
                    if (ImGui::BeginPopupContextItem("cached_column_context_menu")) {
                        if (ImGui::MenuItem("Edit Column")) {
                            getColumnDialog().showEditColumnDialog(db, table->name,
                                                                   table->columns[colIndex]);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Drop Column")) {
                            // TODO: Implement drop column functionality
                            LogPanel::info("Drop column functionality not yet implemented");
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }

    void renderCachedViewNode(const std::shared_ptr<DatabaseInterface> &db,
                              const std::string &dbName, int viewIndex) {
        auto &app = Application::getInstance();

        // Get the cached view data
        const Table *view = nullptr;
        if (db->getType() == DatabaseType::POSTGRESQL) {
            const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
            const auto &dbData = pgDb->getDatabaseData(dbName);
            if (viewIndex < dbData.views.size()) {
                view = &dbData.views[viewIndex];
            }
        } else if (db->getType() == DatabaseType::MYSQL) {
            const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
            const auto &dbData = mysqlDb->getDatabaseData(dbName);
            if (viewIndex < dbData.views.size()) {
                view = &dbData.views[viewIndex];
            }
        }

        if (!view)
            return;

        constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string viewLabel = std::format("   {}###cached_view_{}_{}", view->name, dbName,
                                                  view->name); // 3 spaces for icon with unique ID
        ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

        // Draw colored icon over the placeholder space
        const ImVec2 viewIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            viewIconPos, ImGui::GetColorU32(ImVec4(0.9f, 0.6f, 0.2f, 1.0f)), // Orange for views
            ICON_FA_EYE);

        const auto &tabManager = app.getTabManager();
        // Double-click to open view viewer
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            // Auto-switch to the correct database before opening view viewer
            if (db->getType() == DatabaseType::POSTGRESQL) {
                const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (pgDb && dbName != pgDb->getDatabaseName()) {
                    LogPanel::debug("Auto-switching to database: " + dbName +
                                    " to view: " + view->name);
                    auto [success, error] = pgDb->switchToDatabase(dbName);
                    if (!success) {
                        LogPanel::error("Failed to switch database: " + error);
                        return;
                    }
                }
            } else if (db->getType() == DatabaseType::MYSQL) {
                const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
                if (mysqlDb && dbName != mysqlDb->getDatabaseName()) {
                    LogPanel::debug("Auto-switching to database: " + dbName +
                                    " to view: " + view->name);
                    auto [success, error] = mysqlDb->switchToDatabase(dbName);
                    if (!success) {
                        LogPanel::error("Failed to switch database: " + error);
                        return;
                    }
                }
            }
            tabManager->createTableViewerTab(db->getConnectionString(), view->name);
        }

        // Context menu
        ImGui::PushID(viewIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                // Auto-switch to the correct database before opening view viewer
                if (db->getType() == DatabaseType::POSTGRESQL) {
                    const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
                    if (pgDb && dbName != pgDb->getDatabaseName()) {
                        LogPanel::debug("Auto-switching to database: " + dbName +
                                        " to view: " + view->name);
                        auto [success, error] = pgDb->switchToDatabase(dbName);
                        if (!success) {
                            LogPanel::error("Failed to switch database: " + error);
                            ImGui::EndPopup();
                            ImGui::PopID();
                            return;
                        }
                    }
                } else if (db->getType() == DatabaseType::MYSQL) {
                    const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
                    if (mysqlDb && dbName != mysqlDb->getDatabaseName()) {
                        LogPanel::debug("Auto-switching to database: " + dbName +
                                        " to view: " + view->name);
                        auto [success, error] = mysqlDb->switchToDatabase(dbName);
                        if (!success) {
                            LogPanel::error("Failed to switch database: " + error);
                            ImGui::EndPopup();
                            ImGui::PopID();
                            return;
                        }
                    }
                }
                tabManager->createTableViewerTab(db->getConnectionString(), view->name);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
} // namespace HierarchyHelpers
