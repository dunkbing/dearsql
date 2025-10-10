#include "ui/postgres_hierarchy.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"

namespace {
    // PostgreSQL-specific icon colors
    void renderPostgresDatabaseIcon() {
        HierarchyHelpers::renderTreeNodeIcon(ICON_FK_DATABASE, ImVec4(0.2f, 0.6f, 0.9f, 1.0f));
    }

    void renderSchemaNodeIcon() {
        HierarchyHelpers::renderTreeNodeIcon(ICON_FA_FOLDER, ImVec4(0.7f, 0.5f, 0.9f, 1.0f));
    }

    void renderSequenceNodeIcon() {
        HierarchyHelpers::renderTreeNodeIcon(ICON_FA_LIST_OL, ImVec4(0.8f, 0.3f, 0.8f, 1.0f));
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
        ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        const std::string actualDbName = pgDb->getDatabaseName();

        // Keep the node expanded if it was previously expanded
        if (pgDb->isDatabaseExpanded(actualDbName)) {
            dbNodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

        std::string dbDisplayName = actualDbName;
        if (pgDb->areSchemasLoaded() && !pgDb->getSchemas().empty()) {
            dbDisplayName = std::format("{} ({} schemas)", actualDbName, pgDb->getSchemas().size());
        }
        const std::string dbNodeLabel =
            HierarchyHelpers::makeTreeNodeLabel(dbDisplayName, "db_single_" + actualDbName);
        const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

        renderPostgresDatabaseIcon();

        // Context menu for database node
        if (ImGui::BeginPopupContextItem("db_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", pgDb, pgDb->getDatabaseName());
                Logger::debug("Creating new SQL editor for database: " + pgDb->getDatabaseName());
            }
            if (ImGui::MenuItem("Show Diagram")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createDiagramTab(pgDb, pgDb->getDatabaseName());
                Logger::debug("Creating diagram for database: " + pgDb->getDatabaseName());
            }
            ImGui::EndPopup();
        }

        if (dbNodeOpen) {
            if (!pgDb->isDatabaseExpanded(actualDbName)) {
                pgDb->setDatabaseExpanded(actualDbName, true);
            }
            // Load schemas when database node is opened
            if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                Logger::debug(
                    "Database node expanded and schemas not loaded yet, attempting to load...");
                pgDb->refreshSchemas();
            }

            // Show schemas
            renderSchemasSection(pgDb);

            ImGui::TreePop();
        } else if (pgDb->isDatabaseExpanded(actualDbName)) {
            pgDb->setDatabaseExpanded(actualDbName, false);
        }
    }

    void renderAllDatabasesHierarchy(const std::shared_ptr<PostgresDatabase>& pgDb) {
        // Check for async database loading completion
        if (pgDb->isLoadingDatabases()) {
            pgDb->checkDatabasesStatusAsync();
        }

        // Check schema loading status for all expanded databases
        const auto& databases = pgDb->getDatabaseDataMap();
        for (const auto& [dbName, dbData] : databases) {
            if (pgDb->isDatabaseExpanded(dbName)) {
                if (dbData.loadingSchemas) {
                    pgDb->checkSchemasStatusAsync(dbName);
                }
            }
        }

        // Show loading indicator if databases are being loaded
        if (pgDb->isLoadingDatabases() && databases.empty()) {
            HierarchyHelpers::renderLoadingState("Loading databases...",
                                                 "##loading_databases_spinner");
            return;
        }

        if (databases.empty() && !pgDb->isLoadingDatabases()) {
            ImGui::Text("  No databases found");
            return;
        }

        for (const auto& [dbName, dbData] : databases) {
            ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Keep the node expanded if it was previously expanded
            const bool shouldBeExpanded = pgDb->isDatabaseExpanded(dbName);

            if (shouldBeExpanded) {
                dbNodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string dbNodeLabel =
                HierarchyHelpers::makeTreeNodeLabel(dbName, "db_" + dbName);

            // Force the node open every frame if it should be expanded
            // This prevents ImGui from collapsing it when content changes (e.g., schemas loading)
            if (shouldBeExpanded) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

            // Detect if user clicked to toggle this node
            const bool wasClicked = ImGui::IsItemClicked();

            renderPostgresDatabaseIcon();

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createSQLEditorTab("", pgDb, dbName);
                    Logger::debug("Creating new SQL editor for database: " + dbName);
                }
                if (ImGui::MenuItem("Show Diagram")) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createDiagramTab(pgDb, dbName);
                    Logger::debug("Creating diagram for database: " + dbName);
                }
                ImGui::EndPopup();
            }

            if (dbNodeOpen) {
                // Track that this database is expanded (do this immediately)
                pgDb->setDatabaseExpanded(dbName, true);

                // Only switch database if we're not already connected to it
                if (dbName != pgDb->getDatabaseName()) {
                    if (!pgDb->isSwitchingDatabase()) {
                        Logger::debug("Starting async switch to database: " + dbName);
                        pgDb->switchToDatabaseAsync(dbName);
                    }
                }

                // Show schemas for this database (cached or load if needed)
                if (dbName == pgDb->getDatabaseName()) {
                    // Load schemas only when first expanded and not already loaded
                    if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                        Logger::debug("Database " + dbName +
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
                // Node is reported as closed
                // Only clear expanded state if user actually clicked to collapse it
                if (wasClicked && pgDb->isDatabaseExpanded(dbName)) {
                    Logger::debug("Database node " + dbName + " collapsed by user click");
                    pgDb->setDatabaseExpanded(dbName, false);
                }
                // Otherwise, trust SetNextItemOpen to reopen it next frame
            }
        }
    }

    void renderSchemasSection(const std::shared_ptr<PostgresDatabase>& pgDb) {
        if (pgDb->getSchemas().empty()) {
            if (pgDb->isSwitchingDatabase()) {
                HierarchyHelpers::renderLoadingState("Connecting...", "##connecting_spinner");
            } else if (pgDb->isLoadingSchemas()) {
                HierarchyHelpers::renderLoadingState("Loading schemas...",
                                                     "##loading_schemas_spinner");
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
                HierarchyHelpers::renderLoadingState("Loading schemas...",
                                                     "##loading_schemas_spinner");
            } else if (!dbData.schemasLoaded) {
                // Start parallel schema loading for this database
                if (dbName != pgDb->getDatabaseName()) {
                    pgDb->startSchemasLoadAsync(dbName);
                } else {
                    pgDb->refreshSchemas();
                }
                HierarchyHelpers::renderLoadingState("Loading schemas...",
                                                     "##loading_schemas_spinner");
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

    void renderSchemaTablesSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                   const std::string& schemaName, const std::string& dbName) {
        const bool targetsCurrentDatabase = dbName.empty() || dbName == pgDb->getDatabaseName();

        auto& schemaData = targetsCurrentDatabase ? pgDb->getSchemaData(schemaName)
                                                  : pgDb->getSchemaData(dbName, schemaName);

        const std::string databaseId = targetsCurrentDatabase ? pgDb->getDatabaseName() : dbName;

        ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        if (schemaData.tablesExpanded) {
            tablesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string tablesLabel = HierarchyHelpers::makeTreeNodeLabel(
            std::format("Tables ({})", schemaData.tables.size()),
            std::format("tables_{}_{}", databaseId, schemaName));
        const bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

        schemaData.tablesExpanded = tablesOpen;

        if (!databaseId.empty()) {
            if (targetsCurrentDatabase) {
                pgDb->getCurrentDatabaseData().tablesExpanded = tablesOpen;
            } else {
                pgDb->getDatabaseData(databaseId).tablesExpanded = tablesOpen;
            }
        }

        HierarchyHelpers::renderTreeNodeIcon(ICON_FA_TABLE, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));

        if (schemaData.loadingTables) {
            ImGui::SameLine();
            UIUtils::Spinner(("##tables_spinner_" + databaseId + "_" + schemaName).c_str(), 6.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
        }

        const bool canLoadTables = targetsCurrentDatabase && !schemaName.empty();
        if (tablesOpen && canLoadTables && !schemaData.tablesLoaded && !schemaData.loadingTables) {
            Logger::debug("Tables node expanded for schema " + schemaName + ", loading tables...");
            pgDb->refreshTables(schemaName);
        }

        if (tablesOpen) {
            if (schemaData.tables.empty()) {
                if (schemaData.loadingTables) {
                    HierarchyHelpers::renderLoadingState(
                        "Loading tables...",
                        ("##loading_tables_" + databaseId + "_" + schemaName).c_str());
                } else if (!schemaData.tablesLoaded) {
                    ImGui::Text(targetsCurrentDatabase ? "  Loading..." : "  Not loaded yet");
                } else {
                    ImGui::Text("  No tables found");
                }
            } else {
                const std::string dbNameForSwitch =
                    targetsCurrentDatabase ? pgDb->getDatabaseName() : databaseId;
                for (auto& table : schemaData.tables) {
                    HierarchyHelpers::renderTableLeafItem(pgDb, table, schemaName, dbNameForSwitch);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderSchemaTablesSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                   const std::string& schemaName) {
        renderSchemaTablesSection(pgDb, schemaName, "");
    }

    void renderSchemaViewsSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                  const std::string& schemaName, const std::string& dbName) {
        const bool targetsCurrentDatabase = dbName.empty() || dbName == pgDb->getDatabaseName();

        auto& schemaData = targetsCurrentDatabase ? pgDb->getSchemaData(schemaName)
                                                  : pgDb->getSchemaData(dbName, schemaName);

        const std::string databaseId = targetsCurrentDatabase ? pgDb->getDatabaseName() : dbName;

        ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                        ImGuiTreeNodeFlags_FramePadding;

        if (schemaData.viewsExpanded) {
            viewsFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string viewsLabel =
            HierarchyHelpers::makeTreeNodeLabel(std::format("Views ({})", schemaData.views.size()),
                                                std::format("views_{}_{}", databaseId, schemaName));
        const bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

        schemaData.viewsExpanded = viewsOpen;

        if (!databaseId.empty()) {
            if (targetsCurrentDatabase) {
                pgDb->getCurrentDatabaseData().viewsExpanded = viewsOpen;
            } else {
                pgDb->getDatabaseData(databaseId).viewsExpanded = viewsOpen;
            }
        }

        HierarchyHelpers::renderTreeNodeIcon(ICON_FA_EYE, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));

        if (schemaData.loadingViews) {
            ImGui::SameLine();
            UIUtils::Spinner(("##views_spinner_" + databaseId + "_" + schemaName).c_str(), 6.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
        }

        const bool canLoadViews = targetsCurrentDatabase && !schemaName.empty();
        if (viewsOpen && canLoadViews && !schemaData.viewsLoaded && !schemaData.loadingViews) {
            Logger::debug("Views node expanded for schema " + schemaName + ", loading views...");
            pgDb->refreshViews(schemaName);
        }

        if (viewsOpen) {
            if (schemaData.views.empty()) {
                if (schemaData.loadingViews) {
                    HierarchyHelpers::renderLoadingState(
                        "Loading views...",
                        ("##loading_views_" + databaseId + "_" + schemaName).c_str());
                } else if (!schemaData.viewsLoaded) {
                    ImGui::Text(targetsCurrentDatabase ? "  Loading..." : "  Not loaded yet");
                } else {
                    ImGui::Text("  No views found");
                }
            } else {
                const std::string dbNameForSwitch =
                    targetsCurrentDatabase ? pgDb->getDatabaseName() : databaseId;
                for (auto& view : schemaData.views) {
                    HierarchyHelpers::renderViewLeafItem(pgDb, view, schemaName, dbNameForSwitch);
                }
            }
            ImGui::TreePop();
        }
    }

    void renderSchemaViewsSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                  const std::string& schemaName) {
        renderSchemaViewsSection(pgDb, schemaName, "");
    }

    void renderSchemaSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                      const std::string& schemaName, const std::string& dbName) {
        const bool targetsCurrentDatabase = dbName.empty() || dbName == pgDb->getDatabaseName();

        auto& schemaData = targetsCurrentDatabase ? pgDb->getSchemaData(schemaName)
                                                  : pgDb->getSchemaData(dbName, schemaName);

        const std::string databaseId = targetsCurrentDatabase ? pgDb->getDatabaseName() : dbName;

        ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                            ImGuiTreeNodeFlags_FramePadding;

        if (schemaData.sequencesExpanded) {
            sequencesFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        const std::string sequencesLabel = HierarchyHelpers::makeTreeNodeLabel(
            std::format("Sequences ({})", schemaData.sequences.size()),
            std::format("sequences_{}_{}", databaseId, schemaName));
        const bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

        schemaData.sequencesExpanded = sequencesOpen;

        renderSequenceNodeIcon();

        if (schemaData.loadingSequences) {
            ImGui::SameLine();
            UIUtils::Spinner(("##sequences_spinner_" + databaseId + "_" + schemaName).c_str(), 6.0f,
                             2, ImGui::GetColorU32(ImGuiCol_Text));
        }

        const bool canLoadSequences = targetsCurrentDatabase && !schemaName.empty();
        if (sequencesOpen && canLoadSequences && !schemaData.sequencesLoaded &&
            !schemaData.loadingSequences) {
            Logger::debug("Sequences node expanded for schema " + schemaName +
                          ", loading sequences...");
            pgDb->refreshSequences(schemaName);
        }

        if (sequencesOpen) {
            if (schemaData.sequences.empty()) {
                if (schemaData.loadingSequences) {
                    HierarchyHelpers::renderLoadingState(
                        "Loading sequences...",
                        ("##loading_sequences_" + databaseId + "_" + schemaName).c_str());
                } else if (!schemaData.sequencesLoaded) {
                    ImGui::Text(targetsCurrentDatabase ? "  Loading..." : "  Not loaded yet");
                } else {
                    ImGui::Text("  No sequences found");
                }
            } else {
                for (size_t i = 0; i < schemaData.sequences.size(); i++) {
                    const auto& sequence = schemaData.sequences[i];
                    ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                       ImGuiTreeNodeFlags_FramePadding;
                    const std::string sequenceLabel = HierarchyHelpers::makeTreeNodeLabel(
                        sequence, std::format("seq_{}_{}_{}", databaseId, schemaName, sequence));
                    ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);
                    renderSequenceNodeIcon();
                }
            }
            ImGui::TreePop();
        }
    }

    void renderSchemaSequencesSection(const std::shared_ptr<PostgresDatabase>& pgDb,
                                      const std::string& schemaName) {
        renderSchemaSequencesSection(pgDb, schemaName, "");
    }

    void renderSchemaNode(const std::shared_ptr<PostgresDatabase>& pgDb, int schemaIndex) {
        auto& schema = pgDb->getSchemas()[schemaIndex];

        ImGuiTreeNodeFlags schemaFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        if (schema.expanded) {
            schemaFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            // Force the schema to stay open every frame if it should be expanded
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

        const std::string schemaLabel = HierarchyHelpers::makeTreeNodeLabel(
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
        auto* dbData = pgDb->getDatabaseData(dbName);
        if (!dbData || schemaIndex >= dbData->schemas.size() || !dbData->schemas[schemaIndex])
            return;

        auto& schema = *dbData->schemas[schemaIndex];

        ImGuiTreeNodeFlags schemaFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        if (schema.expanded) {
            schemaFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            // Force the schema to stay open every frame if it should be expanded
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

        const std::string schemaLabel = HierarchyHelpers::makeTreeNodeLabel(
            schema.name, std::format("cached_schema_{}_{}", dbName, schema.name));
        bool schemaOpen = ImGui::TreeNodeEx(schemaLabel.c_str(), schemaFlags);

        schema.expanded = schemaOpen;
        renderSchemaNodeIcon();

        if (schemaOpen) {
            // Show cached schema contents preserving per-schema expansion state
            renderSchemaTablesSection(pgDb, schema.name, dbName);
            renderSchemaViewsSection(pgDb, schema.name, dbName);
            renderSchemaSequencesSection(pgDb, schema.name, dbName);

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

        const std::string sequenceLabel = HierarchyHelpers::makeTreeNodeLabel(
            sequence, std::format("sequence_{}_{}", pgDb->getName(), sequence));
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
} // namespace PostgresHierarchy
