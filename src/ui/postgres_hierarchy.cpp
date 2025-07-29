#include "ui/postgres_hierarchy.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include "utils/spinner.hpp"
#include <iostream>

// Forward declarations for helper functions from DatabaseSidebar
namespace {
    void renderSequencesSection(PostgresDatabase *pgDb);
} // namespace

namespace PostgresHierarchy {
    void renderPostgresHierarchy(PostgresDatabase *pgDb) {
        if (pgDb->shouldShowAllDatabases()) {
            // Show all databases from the server
            renderAllDatabasesHierarchy(pgDb);
        } else {
            // Show only the connected database
            renderSingleDatabaseHierarchy(pgDb);
        }
    }

    void renderSingleDatabaseHierarchy(PostgresDatabase *pgDb) {
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

        // Context menu for database node
        if (ImGui::BeginPopupContextItem("db_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto &app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", pgDb->getConnectionString());
                std::cout << "Creating new SQL editor for database: " << pgDb->getDatabaseName()
                          << std::endl;
            }
            ImGui::EndPopup();
        }

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

    void renderAllDatabasesHierarchy(PostgresDatabase *pgDb) {
        // Load databases only once
        if (!pgDb->areDatabasesLoaded()) {
            pgDb->getDatabaseNames(); // This will load and cache the databases
        }

        // Get all databases from the server (cached)
        std::vector<std::string> databases = pgDb->getDatabaseNames();

        for (const auto &dbName : databases) {
            constexpr ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                       ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                       ImGuiTreeNodeFlags_FramePadding;

            // Draw database node with placeholder space for icon
            const std::string dbNodeLabel = std::format("   {}###db_{}", dbName, dbName);
            const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

            // Draw colored icon over the placeholder space
            const auto dbNodeIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

            ImGui::GetWindowDrawList()->AddText(
                dbNodeIconPos,
                ImGui::GetColorU32(ImVec4(0.2f, 0.6f, 0.9f, 1.0f)), // Blue for database
                ICON_FK_DATABASE);

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    auto &app = Application::getInstance();
                    app.getTabManager()->createSQLEditorTab("", pgDb->getConnectionString());
                    std::cout << "Creating new SQL editor for database: " << dbName << std::endl;
                }
                ImGui::EndPopup();
            }

            if (dbNodeOpen) {
                // Track that this database is expanded
                if (!pgDb->isDatabaseExpanded(dbName)) {
                    pgDb->setDatabaseExpanded(dbName, true);

                    // Only switch database if we're not already connected to it
                    if (dbName != pgDb->getDatabaseName()) {
                        std::cout << "Switching to database: " << dbName << std::endl;
                        auto [success, error] = pgDb->switchToDatabase(dbName);
                        if (!success) {
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                               "  Failed to connect to database:");
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  %s",
                                               error.c_str());
                            pgDb->setDatabaseExpanded(dbName,
                                                      false); // Mark as not expanded due to failure
                            ImGui::TreePop();
                            continue;
                        }
                    }
                }

                // Only show schemas if we're currently connected to this database
                if (dbName == pgDb->getDatabaseName()) {
                    // Load schemas only when first expanded and not already loaded
                    if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                        std::cout << "Database " << dbName
                                  << " expanded for first time, loading schemas..." << std::endl;
                        pgDb->refreshSchemas();
                    }

                    // Show schemas
                    renderSchemasSection(pgDb);
                } else {
                    // We're showing a different database, so show placeholder
                    ImGui::Text("  Switch to this database to view contents");
                }

                ImGui::TreePop();
            } else {
                // Node is collapsed, mark as not expanded
                if (pgDb->isDatabaseExpanded(dbName)) {
                    pgDb->setDatabaseExpanded(dbName, false);
                }
            }
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
            HierarchyHelpers::renderTablesSection(pgDb);
            HierarchyHelpers::renderViewsSection(pgDb);
            renderSequencesSection(pgDb);

            ImGui::TreePop();
        }
    }

} // namespace PostgresHierarchy

// Helper functions (these would need to be refactored from DatabaseSidebar as well)
namespace {

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

        // Context menu for Sequences section
        if (ImGui::BeginPopupContextItem("sequences_context_menu")) {
            if (ImGui::MenuItem("Refresh")) {
                pgDb->setSequencesLoaded(false);
                pgDb->refreshSequences();
            }
            ImGui::EndPopup();
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
