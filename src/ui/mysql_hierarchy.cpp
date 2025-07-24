#include "ui/mysql_hierarchy.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "imgui.h"
#include "utils/spinner.hpp"
#include <iostream>

// Forward declarations for helper functions
namespace {
    void renderTablesSection(MySQLDatabase *mysqlDb);
    void renderViewsSection(MySQLDatabase *mysqlDb);
} // namespace

namespace MySQLHierarchy {

    void renderMySQLHierarchy(MySQLDatabase *mysqlDb) {
        renderTablesSection(mysqlDb);
        renderViewsSection(mysqlDb);
    }

    void renderTableNode(MySQLDatabase *mysqlDb, int tableIndex) {
        auto &app = Application::getInstance();
        const auto &table = mysqlDb->getTables()[tableIndex];

        ImGuiTreeNodeFlags tableFlags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string tableLabel = std::format("   {}", table.name); // 3 spaces for icon
        const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

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
            app.getTabManager()->createTableViewerTab(mysqlDb->getConnectionString(), table.name);
        }

        // Context menu
        ImGui::PushID(static_cast<int>(tableIndex));
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(mysqlDb->getConnectionString(),
                                                          table.name);
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
            const ImVec2 columnsIconPos =
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
            const ImVec2 keysIconPos =
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
            const auto indexesIconPos =
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

    void renderViewNode(MySQLDatabase *mysqlDb, const int viewIndex) {
        auto &view = mysqlDb->getViews()[viewIndex];

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

        const auto &app = Application::getInstance();
        const auto tabManager = app.getTabManager();
        // Double-click to open view viewer (async loading will be handled by the tab)
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            tabManager->createTableViewerTab(mysqlDb->getConnectionString(), view.name);
        }

        // Context menu
        ImGui::PushID(viewIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                tabManager->createTableViewerTab(mysqlDb->getConnectionString(), view.name);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

} // namespace MySQLHierarchy

// Helper functions
namespace {
    void renderTablesSection(MySQLDatabase *mysqlDb) {
        constexpr ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Tables node if loading
        const bool showTablesSpinner = mysqlDb->isLoadingTables();

        // Draw tree node with placeholder space for icon
        const std::string tablesLabel =
            std::format("   Tables ({})###tables",
                        mysqlDb->getTables().size()); // 3 spaces for icon, ###tables for stable ID
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
        if (tablesOpen && !mysqlDb->areTablesLoaded() && !mysqlDb->isLoadingTables()) {
            std::cout << "Tables node expanded and tables not loaded yet, attempting to load..."
                      << std::endl;
            mysqlDb->refreshTables();
        }

        if (tablesOpen) {
            if (mysqlDb->getTables().empty()) {
                if (mysqlDb->isLoadingTables()) {
                    // Show loading indicator with spinner
                    ImGui::Text("  Loading tables...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_tables_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (!mysqlDb->areTablesLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No tables found");
                }
            } else {
                for (int j = 0; j < mysqlDb->getTables().size(); j++) {
                    MySQLHierarchy::renderTableNode(mysqlDb, j);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderViewsSection(MySQLDatabase *mysqlDb) {
        constexpr ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                  ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                  ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Views node if loading
        const bool showViewsSpinner = mysqlDb->isLoadingViews();

        // Draw tree node with placeholder space for icon
        const std::string viewsLabel =
            std::format("   Views ({})###views",
                        mysqlDb->getViews().size()); // 3 spaces for icon, ###views for stable ID
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
        if (viewsOpen && !mysqlDb->areViewsLoaded() && !mysqlDb->isLoadingViews()) {
            std::cout << "Views node expanded and views not loaded yet, attempting to load..."
                      << std::endl;
            mysqlDb->refreshViews();
        }

        if (viewsOpen) {
            if (mysqlDb->getViews().empty()) {
                if (mysqlDb->isLoadingViews()) {
                    // Show loading indicator with spinner
                    ImGui::Text("  Loading views...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_views_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (!mysqlDb->areViewsLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No views found");
                }
            } else {
                for (int j = 0; j < mysqlDb->getViews().size(); j++) {
                    MySQLHierarchy::renderViewNode(mysqlDb, j);
                }
            }
            ImGui::TreePop();
        }
    }
} // anonymous namespace
