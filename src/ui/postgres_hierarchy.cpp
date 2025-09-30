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
    void renderSchemaTablesSection(const std::shared_ptr<PostgresDatabase>& pgDb, const std::string& schemaName);
    void renderSchemaViewsSection(const std::shared_ptr<PostgresDatabase>& pgDb, const std::string& schemaName);
    void renderSchemaSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb, const std::string& schemaName);

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

    void renderSchemaTablesSection(const std::shared_ptr<PostgresDatabase>& pgDb, const std::string& schemaName) {
        auto& schemaData = pgDb->getSchemaData(schemaName);

        ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                        ImGuiTreeNodeFlags_FramePadding;

        if (schemaData.tablesExpanded) {
            tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string tablesLabel = makeTreeNodeLabel(
            std::format("Tables ({})", schemaData.tables.size()),
            std::format("tables_{}_{}", pgDb->getName(), schemaName));
        const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

        schemaData.tablesExpanded = tablesOpen;

        renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

        if (schemaData.loadingTables) {
            ImGui::SameLine();
            UIUtils::Spinner(("##tables_spinner_" + schemaName).c_str(), 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load tables when the tree node is opened and tables haven't been loaded yet
        if (tablesOpen && !schemaData.tablesLoaded && !schemaData.loadingTables) {
            LogPanel::debug("Tables node expanded for schema " + schemaName + ", loading tables...");
            pgDb->refreshTables(schemaName);
        }

        if (tablesOpen) {
            if (schemaData.tables.empty()) {
                if (schemaData.loadingTables) {
                    renderLoadingState("Loading tables...", ("##loading_tables_" + schemaName).c_str());
                } else {
                    ImGui::Text("  No tables found");
                }
            } else {
                for (size_t i = 0; i < schemaData.tables.size(); i++) {
                    const auto& table = schemaData.tables[i];
                    ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_Leaf |
                                                   ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                   ImGuiTreeNodeFlags_FramePadding;
                    const std::string tableLabel = makeTreeNodeLabel(table.name, table.fullName);
                    ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);
                    renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
                }
            }
            ImGui::TreePop();
        }
    }

    void renderSchemaViewsSection(const std::shared_ptr<PostgresDatabase>& pgDb, const std::string& schemaName) {
        auto& schemaData = pgDb->getSchemaData(schemaName);

        ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                       ImGuiTreeNodeFlags_FramePadding;

        if (schemaData.viewsExpanded) {
            viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string viewsLabel = makeTreeNodeLabel(
            std::format("Views ({})", schemaData.views.size()),
            std::format("views_{}_{}", pgDb->getName(), schemaName));
        const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

        schemaData.viewsExpanded = viewsOpen;

        renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

        if (schemaData.loadingViews) {
            ImGui::SameLine();
            UIUtils::Spinner(("##views_spinner_" + schemaName).c_str(), 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load views when the tree node is opened and views haven't been loaded yet
        if (viewsOpen && !schemaData.viewsLoaded && !schemaData.loadingViews) {
            LogPanel::debug("Views node expanded for schema " + schemaName + ", loading views...");
            pgDb->refreshViews(schemaName);
        }

        if (viewsOpen) {
            if (schemaData.views.empty()) {
                if (schemaData.loadingViews) {
                    renderLoadingState("Loading views...", ("##loading_views_" + schemaName).c_str());
                } else {
                    ImGui::Text("  No views found");
                }
            } else {
                for (size_t i = 0; i < schemaData.views.size(); i++) {
                    const auto& view = schemaData.views[i];
                    ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    const std::string viewLabel = makeTreeNodeLabel(view.name, view.fullName);
                    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);
                    renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));
                }
            }
            ImGui::TreePop();
        }
    }

    void renderSchemaSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb, const std::string& schemaName) {
        auto& schemaData = pgDb->getSchemaData(schemaName);

        ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                           ImGuiTreeNodeFlags_FramePadding;

        if (schemaData.sequencesExpanded) {
            sequencesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string sequencesLabel = makeTreeNodeLabel(
            std::format("Sequences ({})", schemaData.sequences.size()),
            std::format("sequences_{}_{}", pgDb->getName(), schemaName));
        const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

        schemaData.sequencesExpanded = sequencesOpen;

        renderSequenceNodeIcon();

        if (schemaData.loadingSequences) {
            ImGui::SameLine();
            UIUtils::Spinner(("##sequences_spinner_" + schemaName).c_str(), 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        // Load sequences when the tree node is opened and sequences haven't been loaded yet
        if (sequencesOpen && !schemaData.sequencesLoaded && !schemaData.loadingSequences) {
            LogPanel::debug("Sequences node expanded for schema " + schemaName + ", loading sequences...");
            pgDb->refreshSequences(schemaName);
        }

        if (sequencesOpen) {
            if (schemaData.sequences.empty()) {
                if (schemaData.loadingSequences) {
                    renderLoadingState("Loading sequences...", ("##loading_sequences_" + schemaName).c_str());
                } else {
                    ImGui::Text("  No sequences found");
                }
            } else {
                for (size_t i = 0; i < schemaData.sequences.size(); i++) {
                    const auto& sequence = schemaData.sequences[i];
                    ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                                      ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                      ImGuiTreeNodeFlags_FramePadding;
                    const std::string sequenceLabel = makeTreeNodeLabel(sequence,
                        std::format("seq_{}_{}_{}", pgDb->getName(), schemaName, sequence));
                    ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);
                    renderSequenceNodeIcon();
                }
            }
            ImGui::TreePop();
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

        const std::string schemaLabel = makeTreeNodeLabel(
            schema.name, std::format("schema_{}_{}", pgDb->getName(), schema.name));
        const bool schemaOpen = ImGui::TreeNodeEx(schemaLabel.c_str(), schemaFlags);

        schema.expanded = schemaOpen;
        renderSchemaNodeIcon();

        if (schemaOpen) {
            // Check async loading status for this schema
            pgDb->checkSchemaTablesStatusAsync(schema.name);
            pgDb->checkSchemaViewsStatusAsync(schema.name);
            pgDb->checkSchemaSequencesStatusAsync(schema.name);

            // Render schema-specific sections
            renderSchemaTablesSection(pgDb, schema.name);
            renderSchemaViewsSection(pgDb, schema.name);
            renderSchemaSequencesSection(pgDb, schema.name);

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

    void renderCachedSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                      const std::string& dbName) {
        auto& dbData = pgDb->getDatabaseData(dbName);

        // Aggregate sequences from all schemas in this database
        std::vector<std::string> allSequences;
        bool anyLoaded = false;
        bool anyLoading = false;

        for (const auto& [schemaName, schemaData] : dbData.schemaDataCache) {
            allSequences.insert(allSequences.end(),
                               schemaData.sequences.begin(),
                               schemaData.sequences.end());
            if (schemaData.sequencesLoaded) anyLoaded = true;
            if (schemaData.loadingSequences) anyLoading = true;
        }

        ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

        const std::string sequencesLabel =
            makeTreeNodeLabel(std::format("Sequences ({})", allSequences.size()),
                              "sequences_cached_pg_" + dbName);
        const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

        renderSequenceNodeIcon();

        if (anyLoading) {
            ImGui::SameLine();
            UIUtils::Spinner(("##sequences_spinner_" + dbName).c_str(), 6.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
        }

        if (sequencesOpen) {
            if (allSequences.empty()) {
                if (anyLoading) {
                    renderLoadingState("Loading sequences...",
                                       ("##loading_sequences_spinner_" + dbName).c_str());
                } else if (!anyLoaded) {
                    ImGui::Text("  Not loaded yet");
                } else {
                    ImGui::Text("  No sequences found");
                }
            } else {
                for (size_t j = 0; j < allSequences.size(); j++) {
                    ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                                      ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                      ImGuiTreeNodeFlags_FramePadding;
                    const std::string sequenceLabel = makeTreeNodeLabel(allSequences[j],
                        std::format("seq_cached_{}_{}", dbName, allSequences[j]));
                    ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);
                    renderSequenceNodeIcon();
                }
            }
            ImGui::TreePop();
        }
    }
} // namespace PostgresHierarchy
