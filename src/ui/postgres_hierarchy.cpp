#include "ui/postgres_hierarchy.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "utils/spinner.hpp"
#include <iostream>

// Forward declarations for helper functions from DatabaseSidebar
namespace {
    void renderTablesSection(PostgresDatabase *pgDb);
    void renderViewsSection(PostgresDatabase *pgDb);
    void renderSequencesSection(PostgresDatabase *pgDb);
} // namespace

namespace PostgresHierarchy {
    void renderPostgresHierarchy(PostgresDatabase *pgDb) {
        // First show the connected database as a child node
        constexpr ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        // Draw database node with placeholder space for icon
        std::string dbName = pgDb->getDatabaseName();
        // Add schema count if schemas are loaded
        if (pgDb->areSchemasLoaded() && !pgDb->getSchemas().empty()) {
            dbName = std::format("{} ({} schemas)", dbName, pgDb->getSchemas().size());
        }
        const std::string dbNodeLabel =
            std::format("   {}###db_node", dbName); // 3 spaces for icon, ###db_node for stable ID
        const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

        // Draw colored icon over the placeholder space
        const auto dbNodeIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            dbNodeIconPos, ImGui::GetColorU32(ImVec4(0.2f, 0.6f, 0.9f, 1.0f)), // Blue for database
            ICON_FK_DATABASE);

        if (dbNodeOpen) {
            // Load schemas when database node is opened
            if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                std::cout
                    << "Database node expanded and schemas not loaded yet, attempting to load..."
                    << std::endl;
                pgDb->refreshSchemas();
            }

            // Show schemas
            renderSchemasSection(pgDb);

            ImGui::TreePop();
        }
    }

    void renderSchemasSection(PostgresDatabase *pgDb) {
        if (pgDb->getSchemas().empty()) {
            if (pgDb->isLoadingSchemas()) {
                // Show loading indicator with spinner
                ImGui::Text("  Loading schemas...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_schemas_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            } else if (!pgDb->areSchemasLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No schemas found");
            }
        } else {
            for (int i = 0; i < pgDb->getSchemas().size(); i++) {
                renderSchemaNode(pgDb, i);
            }
        }
    }

    void renderSchemaNode(PostgresDatabase *pgDb, int schemaIndex) {
        auto &schema = pgDb->getSchemas()[schemaIndex];

        ImGuiTreeNodeFlags schemaFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        // Draw schema node with placeholder space for icon
        const std::string schemaLabel = std::format("   {}", schema.name); // 3 spaces for icon
        bool schemaOpen = ImGui::TreeNodeEx(schemaLabel.c_str(), schemaFlags);

        // Draw colored icon over the placeholder space
        const ImVec2 schemaIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            schemaIconPos, ImGui::GetColorU32(ImVec4(0.7f, 0.5f, 0.9f, 1.0f)), // Purple for schema
            ICON_FA_FOLDER);

        if (schemaOpen) {
            // For now, render the old structure under each schema
            // In the future, we'd filter tables/views/sequences by schema
            renderTablesSection(pgDb);
            renderViewsSection(pgDb);
            renderSequencesSection(pgDb);

            ImGui::TreePop();
        }
    }

} // namespace PostgresHierarchy

// Helper functions (these would need to be refactored from DatabaseSidebar as well)
namespace {
    void renderTablesSection(PostgresDatabase *pgDb) {
        constexpr ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Tables node if loading
        const bool showTablesSpinner = pgDb->isLoadingTables();

        // Draw tree node with placeholder space for icon
        const std::string tablesLabel =
            std::format("   Tables ({})###tables",
                        pgDb->getTables().size()); // 3 spaces for icon, ###tables for stable ID
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
        if (tablesOpen && !pgDb->areTablesLoaded() && !pgDb->isLoadingTables()) {
            std::cout << "Tables node expanded and tables not loaded yet, attempting to load..."
                      << std::endl;
            pgDb->refreshTables();
        }

        if (tablesOpen) {
            if (pgDb->getTables().empty()) {
                if (pgDb->isLoadingTables()) {
                    // Show loading indicator with spinner
                    ImGui::Text("  Loading tables...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_tables_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (!pgDb->areTablesLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No tables found");
                }
            } else {
                for (int j = 0; j < pgDb->getTables().size(); j++) {
                    PostgresHierarchy::renderTableNode(pgDb, j);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderViewsSection(PostgresDatabase *pgDb) {
        constexpr ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                  ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                  ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Views node if loading
        const bool showViewsSpinner = pgDb->isLoadingViews();

        // Draw tree node with placeholder space for icon
        const std::string viewsLabel =
            std::format("   Views ({})###views",
                        pgDb->getViews().size()); // 3 spaces for icon, ###views for stable ID
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
        if (viewsOpen && !pgDb->areViewsLoaded() && !pgDb->isLoadingViews()) {
            std::cout << "Views node expanded and views not loaded yet, attempting to load..."
                      << std::endl;
            pgDb->refreshViews();
        }

        if (viewsOpen) {
            if (pgDb->getViews().empty()) {
                if (pgDb->isLoadingViews()) {
                    // Show loading indicator with spinner
                    ImGui::Text("  Loading views...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_views_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (!pgDb->areViewsLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No views found");
                }
            } else {
                for (int j = 0; j < pgDb->getViews().size(); j++) {
                    PostgresHierarchy::renderViewNode(pgDb, j);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderSequencesSection(PostgresDatabase *pgDb) {
        constexpr ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                      ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                      ImGuiTreeNodeFlags_FramePadding;

        // Show loading indicator next to Sequences node if loading
        const bool showSequencesSpinner = pgDb->isLoadingSequences();

        // Draw tree node with placeholder space for icon
        const std::string sequencesLabel = std::format(
            "   Sequences ({})###sequences",
            pgDb->getSequences().size()); // 3 spaces for icon, ###sequences for stable ID
        const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

        // Draw colored icon over the placeholder space
        const auto sequencesSectionIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            sequencesSectionIconPos,
            ImGui::GetColorU32(ImVec4(0.8f, 0.3f, 0.8f, 1.0f)), // Purple for Sequences section
            ICON_FA_LIST_OL);

        // Show spinner next to Sequences node if loading
        if (showSequencesSpinner) {
            ImGui::SameLine();
            UIUtils::Spinner("##sequences_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load sequences when the tree node is opened and sequences haven't been loaded yet
        if (sequencesOpen && !pgDb->areSequencesLoaded() && !pgDb->isLoadingSequences()) {
            std::cout
                << "Sequences node expanded and sequences not loaded yet, attempting to load..."
                << std::endl;
            pgDb->refreshSequences();
        }

        if (sequencesOpen) {
            if (pgDb->getSequences().empty()) {
                if (pgDb->isLoadingSequences()) {
                    // Show loading indicator with spinner
                    ImGui::Text("  Loading sequences...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##loading_sequences_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (!pgDb->areSequencesLoaded()) {
                    ImGui::Text("  Loading...");
                } else {
                    ImGui::Text("  No sequences found");
                }
            } else {
                for (int j = 0; j < pgDb->getSequences().size(); j++) {
                    PostgresHierarchy::renderSequenceNode(pgDb, j);
                }
            }
            ImGui::TreePop();
        }
    }
} // anonymous namespace

// Node rendering functions
namespace PostgresHierarchy {
    void renderTableNode(PostgresDatabase *pgDb, int tableIndex) {
        auto &app = Application::getInstance();
        const auto &table = pgDb->getTables()[tableIndex];

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
            app.getTabManager()->createTableViewerTab(pgDb->getConnectionString(), table.name);
        }

        // Context menu
        ImGui::PushID(static_cast<int>(tableIndex));
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                app.getTabManager()->createTableViewerTab(pgDb->getConnectionString(), table.name);
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

    void renderViewNode(PostgresDatabase *pgDb, const int viewIndex) {
        auto &view = pgDb->getViews()[viewIndex];

        constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string viewLabel = std::format("   {}", view.name); // 3 spaces for icon
        ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

        // Draw colored icon over the placeholder space
        const auto viewIconPos =
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
            tabManager->createTableViewerTab(pgDb->getConnectionString(), view.name);
        }

        // Context menu
        ImGui::PushID(viewIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("View Data")) {
                tabManager->createTableViewerTab(pgDb->getConnectionString(), view.name);
            }
            if (ImGui::MenuItem("Show Structure")) {
                // TODO: Show view structure in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void renderSequenceNode(PostgresDatabase *pgDb, const int sequenceIndex) {
        auto &sequence = pgDb->getSequences()[sequenceIndex];

        constexpr ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

        // Draw tree node with placeholder space for icon
        const std::string sequenceLabel = std::format("   {}", sequence); // 3 spaces for icon
        ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);

        // Draw colored icon over the placeholder space
        const auto sequenceIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            sequenceIconPos,
            ImGui::GetColorU32(ImVec4(0.8f, 0.3f, 0.8f, 1.0f)), // Purple for sequences
            ICON_FA_LIST_OL);

        // Context menu
        ImGui::PushID(sequenceIndex);
        if (ImGui::BeginPopupContextItem(nullptr)) {
            if (ImGui::MenuItem("Show Details")) {
                // TODO: Show sequence details in a tab
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

} // namespace PostgresHierarchy
