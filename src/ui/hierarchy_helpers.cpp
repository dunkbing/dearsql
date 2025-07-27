#include "ui/hierarchy_helpers.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "utils/spinner.hpp"
#include <iostream>

namespace HierarchyHelpers {
    void renderTableNode(DatabaseInterface *db, int tableIndex) {
        auto &app = Application::getInstance();
        const auto &table = db->getTables()[tableIndex];

        ImGuiTreeNodeFlags tableFlags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string tableLabel = std::format("   {}", table.name); // 3 spaces for icon
        const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

        // Draw colored icon over the placeholder space
        const ImVec2 tableIconPos =
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
        ImGui::PushID(static_cast<int>(tableIndex));
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
                columnsIconPos,
                ImGui::GetColorU32(ImVec4(0.5f, 0.9f, 0.5f, 1.0f)), // Light green for Columns
                ICON_FA_TABLE_COLUMNS);

            if (columnsOpened) {
                for (const auto &[name, type, isPrimaryKey, isNotNull] : table.columns) {
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

                    ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);
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

    void renderViewNode(DatabaseInterface *db, const int viewIndex) {
        auto &app = Application::getInstance();
        const auto &view = db->getViews()[viewIndex];

        constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string viewLabel = std::format("   {}", view.name); // 3 spaces for icon
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

    void renderTablesSection(DatabaseInterface *db) {
        constexpr ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Tables node if loading
        const bool showTablesSpinner = db->isLoadingTables();

        // Draw tree node with placeholder space for icon
        const std::string tablesLabel =
            std::format("   Tables ({})###tables",
                        db->getTables().size()); // 3 spaces for icon, ###tables for stable ID
        const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

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
                for (int j = 0; j < db->getTables().size(); j++) {
                    renderTableNode(db, j);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderViewsSection(DatabaseInterface *db) {
        constexpr ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                  ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                  ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Views node if loading
        const bool showViewsSpinner = db->isLoadingViews();

        // Draw tree node with placeholder space for icon
        const std::string viewsLabel =
            std::format("   Views ({})###views",
                        db->getViews().size()); // 3 spaces for icon, ###views for stable ID
        const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

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
            std::cout << "Views node expanded and views not loaded yet, attempting to load..."
                      << std::endl;
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
    void renderRedisHierarchy(std::shared_ptr<DatabaseInterface> db) {
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

        // Keys section - use the existing table rendering but adapted for Redis
        constexpr ImGuiTreeNodeFlags keysFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                 ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Keys node if loading
        const bool showKeysSpinner = redisDb->isLoadingTables();

        // Draw tree node with placeholder space for icon
        const std::string keysLabel = "   Keys###redis_keys"; // 3 spaces for icon
        const bool keysOpen = ImGui::TreeNodeEx(keysLabel.c_str(), keysFlags);

        // Draw colored icon over the placeholder space
        const auto keysSectionIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            keysSectionIconPos,
            ImGui::GetColorU32(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)), // Gold for Redis Keys
            ICON_FA_KEY);

        // Show spinner next to Keys node if loading
        if (showKeysSpinner) {
            ImGui::SameLine();
            UIUtils::Spinner("##redis_keys_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load keys when the tree node is opened and keys haven't been loaded yet
        if (keysOpen && !redisDb->areTablesLoaded() && !redisDb->isLoadingTables()) {
            std::cout << "Redis keys node expanded and keys not loaded yet, attempting to load..."
                      << std::endl;
            redisDb->refreshTables();
        }

        if (keysOpen) {
            if (redisDb->getTables().empty()) {
                if (redisDb->isLoadingTables()) {
                    ImGui::Text("  Loading keys...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_redis_keys_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (!redisDb->areTablesLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No keys found");
                }
            } else {
                // Show "All Keys" as a clickable item
                const auto &tables = redisDb->getTables();
                for (int i = 0; i < tables.size(); i++) {
                    const auto &table = tables[i];

                    constexpr ImGuiTreeNodeFlags keyGroupFlags =
                        ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                        ImGuiTreeNodeFlags_FramePadding;

                    // Draw tree node with placeholder space for icon
                    const std::string keyGroupLabel =
                        std::format("   {}", table.name); // 3 spaces for icon
                    ImGui::TreeNodeEx(keyGroupLabel.c_str(), keyGroupFlags);

                    // Draw colored icon over the placeholder space
                    const ImVec2 keyGroupIconPos = ImVec2(
                        ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                        ImGui::GetItemRectMin().y +
                            (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

                    ImGui::GetWindowDrawList()->AddText(
                        keyGroupIconPos,
                        ImGui::GetColorU32(
                            ImVec4(0.9f, 0.4f, 0.4f, 1.0f)), // Red for Redis key groups
                        ICON_FA_FOLDER);

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
                            app.getTabManager()->createTableViewerTab(
                                redisDb->getConnectionString(), table.name);
                        }
                        if (ImGui::MenuItem("Refresh Keys")) {
                            redisDb->setTablesLoaded(false);
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
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
} // namespace HierarchyHelpers
