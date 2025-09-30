#include "ui/postgres_hierarchy.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include "ui/log_panel.hpp"
#include "utils/spinner.hpp"

// Forward declarations for helper functions from DatabaseSidebar
namespace {
    void renderSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb);

    // Helper functions to reduce duplication
    void renderTreeNodeIcon(const char* icon, const ImVec4& color) {
        const auto iconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(color), icon);
    }

    std::string makeTreeNodeLabel(const std::string& text, const std::string& id = "") {
        if (id.empty()) {
            return std::format("   {}", text);
        }
        return std::format("   {}###{}", text, id);
    }

    void renderLoadingState(const char* message, const char* spinnerId) {
        ImGui::Text("  %s", message);
        ImGui::SameLine();
        UIUtils::Spinner(spinnerId, 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    void renderDatabaseNodeIcon() {
        renderTreeNodeIcon(ICON_FK_DATABASE, ImVec4(0.2f, 0.6f, 0.9f, 1.0f));
    }

    void renderSchemaNodeIcon() {
        renderTreeNodeIcon(ICON_FA_FOLDER, ImVec4(0.7f, 0.5f, 0.9f, 1.0f));
    }

    void renderSequenceNodeIcon() {
        renderTreeNodeIcon(ICON_FA_LIST_OL, ImVec4(0.8f, 0.3f, 0.8f, 1.0f));
    }
} // namespace

namespace PostgresHierarchy {
    void renderPostgresHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb) {
        if (pgDb->shouldShowAllDatabases()) {
            // Show all databases from the server
            renderAllDatabasesHierarchy(pgDb);
        } else {
            // Show only the connected database
            renderSingleDatabaseHierarchy(pgDb);
        }
    }

    void renderSingleDatabaseHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb) {
        // First show the connected database as a child node
        constexpr ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        std::string dbName = pgDb->getDatabaseName();
        if (pgDb->areSchemasLoaded() && !pgDb->getSchemas().empty()) {
            dbName = std::format("{} ({} schemas)", dbName, pgDb->getSchemas().size());
        }
        const std::string dbNodeLabel = makeTreeNodeLabel(dbName, "db_single_" + dbName);
        const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

        renderDatabaseNodeIcon();

        // Context menu for database node
        if (ImGui::BeginPopupContextItem("db_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", pgDb, pgDb->getDatabaseName());
                LogPanel::debug("Creating new SQL editor for database: " + pgDb->getDatabaseName());
            }
            if (ImGui::MenuItem("Show Diagram")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createDiagramTab(pgDb, pgDb->getDatabaseName());
                LogPanel::debug("Creating diagram for database: " + pgDb->getDatabaseName());
            }
            ImGui::EndPopup();
        }

        if (dbNodeOpen) {
            // Load schemas when database node is opened
            if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                LogPanel::debug(
                    "Database node expanded and schemas not loaded yet, attempting to load...");
                pgDb->refreshSchemas();
            }

            // Show schemas
            renderSchemasSection(pgDb);

            ImGui::TreePop();
        }
    }

    void renderAllDatabasesHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb) {
        // Check for async database loading completion
        if (pgDb->isLoadingDatabases()) {
            pgDb->checkDatabasesStatusAsync();
        }

        // Check schema loading status for all expanded databases
        const std::vector<std::string> databases = pgDb->getDatabaseNames();
        for (const auto& dbName : databases) {
            if (pgDb->isDatabaseExpanded(dbName)) {
                const auto& dbData = pgDb->getDatabaseData(dbName);
                if (dbData.loadingSchemas) {
                    pgDb->checkSchemasStatusAsync(dbName);
                }
            }
        }

        // Show loading indicator if databases are being loaded
        if (pgDb->isLoadingDatabases() && databases.empty()) {
            renderLoadingState("Loading databases...", "##loading_databases_spinner");
            return;
        }

        if (databases.empty() && !pgDb->isLoadingDatabases()) {
            ImGui::Text("  No databases found");
            return;
        }

        for (const auto& dbName : databases) {
            ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Keep the node expanded if it was previously expanded
            if (pgDb->isDatabaseExpanded(dbName)) {
                dbNodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
                // Also explicitly set the next item to be open to prevent collapse
                ImGui::SetNextItemOpen(true);
            }

            const std::string dbNodeLabel = makeTreeNodeLabel(dbName, "db_" + dbName);
            const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

            renderDatabaseNodeIcon();

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createSQLEditorTab("", pgDb, dbName);
                    LogPanel::debug("Creating new SQL editor for database: " + dbName);
                }
                if (ImGui::MenuItem("Show Diagram")) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createDiagramTab(pgDb, dbName);
                    LogPanel::debug("Creating diagram for database: " + dbName);
                }
                ImGui::EndPopup();
            }

            if (dbNodeOpen) {
                // Track that this database is expanded
                if (!pgDb->isDatabaseExpanded(dbName)) {
                    pgDb->setDatabaseExpanded(dbName, true);

                    // Only switch database if we're not already connected to it
                    if (dbName != pgDb->getDatabaseName()) {
                        if (!pgDb->isSwitchingDatabase()) {
                            LogPanel::debug("Starting async switch to database: " + dbName);
                            pgDb->switchToDatabaseAsync(dbName);
                        }
                    }
                }

                // Show schemas for this database (cached or load if needed)
                if (dbName == pgDb->getDatabaseName()) {
                    // Load schemas only when first expanded and not already loaded
                    if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                        LogPanel::debug("Database " + dbName +
                                        " expanded for first time, loading schemas...");
                        pgDb->refreshSchemas();
                    }
                    // Show schemas for currently connected database
                    renderSchemasSection(pgDb);
                } else {
                    // Show cached schemas for other databases
                    renderCachedSchemasSection(pgDb, dbName);
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

    void renderSchemasSection(const std::shared_ptr<PostgresDatabase>& pgDb) {
        if (pgDb->getSchemas().empty()) {
            if (pgDb->isSwitchingDatabase()) {
                renderLoadingState("Connecting...", "##connecting_spinner");
            } else if (pgDb->isLoadingSchemas()) {
                renderLoadingState("Loading schemas...", "##loading_schemas_spinner");
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

    void renderCachedSchemasSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                    const std::string& dbName) {
        // Get cached data for this specific database
        const auto& dbData = pgDb->getDatabaseData(dbName);

        if (dbData.schemas.empty()) {
            if (dbData.loadingSchemas) {
                renderLoadingState("Loading schemas...", "##loading_schemas_spinner");
            } else if (!dbData.schemasLoaded) {
                // Start parallel schema loading for this database
                if (dbName != pgDb->getDatabaseName()) {
                    pgDb->startSchemasLoadAsync(dbName);
                } else {
                    pgDb->refreshSchemas();
                }
                renderLoadingState("Loading schemas...", "##loading_schemas_spinner");
            } else {
                ImGui::Text("  No schemas found");
            }
        } else {
            // Show cached schemas
            for (int i = 0; i < dbData.schemas.size(); i++) {
                renderCachedSchemaNode(pgDb, dbName, i);
            }
        }
    }

    void renderSchemaNode(const std::shared_ptr<PostgresDatabase>& pgDb, int schemaIndex) {
        auto& schema = pgDb->getSchemas()[schemaIndex];

        ImGuiTreeNodeFlags schemaFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        if (schema.expanded) {
            schemaFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string schemaLabel =
            makeTreeNodeLabel(schema.name, std::format("schema_{}_{}", pgDb->getName(), schema.name));
        const bool schemaOpen = ImGui::TreeNodeEx(schemaLabel.c_str(), schemaFlags);

        schema.expanded = schemaOpen;
        renderSchemaNodeIcon();

        if (schemaOpen) {
            // For now, render the old structure under each schema
            // In the future, we'd filter tables/views/sequences by schema
            HierarchyHelpers::renderTablesSection(pgDb);
            HierarchyHelpers::renderViewsSection(pgDb);
            renderSequencesSection(pgDb);

            ImGui::TreePop();
        }
    }

    void renderCachedSchemaNode(const std::shared_ptr<PostgresDatabase>& pgDb,
                                const std::string& dbName, int schemaIndex) {
        auto& dbData = pgDb->getDatabaseData(dbName);
        auto& schema = dbData.schemas[schemaIndex];

        ImGuiTreeNodeFlags schemaFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        if (schema.expanded) {
            schemaFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string schemaLabel =
            makeTreeNodeLabel(schema.name, std::format("cached_schema_{}_{}", dbName, schema.name));
        bool schemaOpen = ImGui::TreeNodeEx(schemaLabel.c_str(), schemaFlags);

        schema.expanded = schemaOpen;
        renderSchemaNodeIcon();

        if (schemaOpen) {
            // Show cached tables/views/sequences for this database
            HierarchyHelpers::renderCachedTablesSection(pgDb, dbName);
            HierarchyHelpers::renderCachedViewsSection(pgDb, dbName);
            renderCachedSequencesSection(pgDb, dbName);

            ImGui::TreePop();
        }
    }

} // namespace PostgresHierarchy

namespace {
    void renderSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb) {
        const bool sequencesExpanded = pgDb->getCurrentDatabaseData().sequencesExpanded;

        ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

        // Set the default open state based on the expansion state
        if (sequencesExpanded) {
            sequencesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const bool showSequencesSpinner = pgDb->isLoadingSequences();

        const std::string sequencesLabel = makeTreeNodeLabel(
            std::format("Sequences ({})", pgDb->getSequences().size()),
            "sequences_current_" + pgDb->getName());
        const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

        pgDb->getCurrentDatabaseData().sequencesExpanded = sequencesOpen;

        renderSequenceNodeIcon();

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
            LogPanel::debug(
                "Sequences node expanded and sequences not loaded yet, attempting to load...");
            pgDb->refreshSequences();
        }

        if (sequencesOpen) {
            if (pgDb->getSequences().empty()) {
                if (pgDb->isLoadingSequences()) {
                    renderLoadingState("Loading sequences...", "##loading_sequences_spinner");
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
    void renderSequenceNode(const std::shared_ptr<PostgresDatabase>& pgDb,
                            const int sequenceIndex) {
        auto& sequence = pgDb->getSequences()[sequenceIndex];

        constexpr ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

        const std::string sequenceLabel =
            makeTreeNodeLabel(sequence, std::format("sequence_{}_{}", pgDb->getName(), sequence));
        ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);

        renderSequenceNodeIcon();

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

    void renderCachedSequenceNode(const std::shared_ptr<PostgresDatabase>& pgDb,
                                  const std::string& dbName, const int sequenceIndex) {
        const auto& dbData = pgDb->getDatabaseData(dbName);
        const auto& sequence = dbData.sequences[sequenceIndex];

        constexpr ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

        const std::string sequenceLabel =
            makeTreeNodeLabel(sequence, std::format("cached_sequence_{}_{}", dbName, sequence));
        ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);

        renderSequenceNodeIcon();

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

    void renderCachedSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                      const std::string& dbName) {
        auto& dbData = pgDb->getDatabaseData(dbName);

        ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

        // Set the default open state based on the expansion state
        if (dbData.sequencesExpanded) {
            sequencesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const bool showSequencesSpinner = dbData.loadingSequences;

        const std::string sequencesLabel = makeTreeNodeLabel(
            std::format("Sequences ({})", dbData.sequences.size()),
            "sequences_cached_pg_" + dbName);
        const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

        dbData.sequencesExpanded = sequencesOpen;

        renderSequenceNodeIcon();

        if (showSequencesSpinner) {
            ImGui::SameLine();
            UIUtils::Spinner(("##sequences_spinner_" + dbName).c_str(), 6.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
        }

        if (sequencesOpen) {
            if (dbData.sequences.empty()) {
                if (dbData.loadingSequences) {
                    renderLoadingState("Loading sequences...",
                                       ("##loading_sequences_spinner_" + dbName).c_str());
                } else if (!dbData.sequencesLoaded) {
                    // Auto-switch database and load sequences when node is expanded
                    if (dbName != pgDb->getDatabaseName()) {
                        if (!pgDb->isSwitchingDatabase()) {
                            LogPanel::debug("Auto-switching to database: " + dbName +
                                            " to load sequences");
                            pgDb->switchToDatabaseAsync(dbName);
                        }
                        ImGui::Text("  Switching database...");
                    } else {
                        pgDb->refreshSequences();
                    }
                    ImGui::Text("  Loading sequences...");
                } else {
                    ImGui::Text("  No sequences found");
                }
            } else {
                for (int j = 0; j < dbData.sequences.size(); j++) {
                    PostgresHierarchy::renderCachedSequenceNode(pgDb, dbName, j);
                }
            }
            ImGui::TreePop();
        }
    }
} // namespace PostgresHierarchy
