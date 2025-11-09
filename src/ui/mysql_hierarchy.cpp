// deprecated
#include "ui/mysql_hierarchy.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include <iostream>

namespace {
    // MySQL-specific database icon color
    void renderMySQLDatabaseIcon() {
        HierarchyHelpers::renderTreeNodeIcon(ICON_FK_DATABASE, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
    }
} // namespace

namespace MySQLHierarchy {
    void renderMySQLHierarchy(const std::shared_ptr<MySQLDatabase>& mysqlDb) {
        if (mysqlDb->shouldShowAllDatabases()) {
            // Show all databases from the server
            renderAllDatabasesHierarchy(mysqlDb);
        } else {
            // Show only the connected database
            renderSingleDatabaseHierarchy(mysqlDb);
        }
    }

    void renderSingleDatabaseHierarchy(const std::shared_ptr<MySQLDatabase>& mysqlDb) {
        // First show the connected database as a child node
        ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                         ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                         ImGuiTreeNodeFlags_FramePadding;

        // Keep the node expanded if it was previously expanded
        std::string actualDbName = mysqlDb->getDatabaseName();
        if (mysqlDb->isDatabaseExpanded(actualDbName)) {
            dbNodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            // Also explicitly set the next item to be open to prevent collapse
            ImGui::SetNextItemOpen(true);
        }

        std::string dbName = actualDbName;
        if (mysqlDb->areTablesLoaded() && !mysqlDb->getTables().empty()) {
            dbName = std::format("{} ({} tables)", dbName, mysqlDb->getTables().size());
        }
        const std::string dbNodeLabel =
            HierarchyHelpers::makeTreeNodeLabel(dbName, "db_single_" + actualDbName);
        const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

        renderMySQLDatabaseIcon();

        // Context menu for database node
        if (ImGui::BeginPopupContextItem("db_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", mysqlDb, mysqlDb->getDatabaseName());
                std::cout << "Creating new SQL editor for database: " << mysqlDb->getDatabaseName()
                          << std::endl;
            }
            if (ImGui::MenuItem("Show Diagram")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createDiagramTab(mysqlDb, mysqlDb->getDatabaseName());
                std::cout << "Creating diagram for database: " << mysqlDb->getDatabaseName()
                          << std::endl;
            }
            ImGui::EndPopup();
        }

        if (dbNodeOpen) {
            // Track that this database is expanded
            if (!mysqlDb->isDatabaseExpanded(actualDbName)) {
                mysqlDb->setDatabaseExpanded(actualDbName, true);
            }

            // Show tables and views (loading is triggered inside when nodes are expanded)
            HierarchyHelpers::renderTablesSection(mysqlDb);
            HierarchyHelpers::renderViewsSection(mysqlDb);

            ImGui::TreePop();
        } else {
            // Node is collapsed, mark as not expanded
            if (mysqlDb->isDatabaseExpanded(actualDbName)) {
                mysqlDb->setDatabaseExpanded(actualDbName, false);
            }
        }
    }

    void renderAllDatabasesHierarchy(const std::shared_ptr<MySQLDatabase>& mysqlDb) {
        // Check for async database loading completion
        if (mysqlDb->isLoadingDatabases()) {
            mysqlDb->checkDatabasesStatusAsync();
        }

        // Get all databases from the server (may trigger async loading)
        std::vector<std::string> databases = mysqlDb->getDatabaseNames();

        // Show loading indicator if databases are being loaded
        if (mysqlDb->isLoadingDatabases() && databases.empty()) {
            HierarchyHelpers::renderLoadingState("Loading databases...",
                                                 "##loading_databases_spinner");
            return;
        }

        if (databases.empty() && !mysqlDb->isLoadingDatabases()) {
            ImGui::Text("  No databases found");
            return;
        }

        for (const auto& dbName : databases) {
            ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Keep the node expanded if it was previously expanded
            const bool shouldBeExpanded = mysqlDb->isDatabaseExpanded(dbName);
            if (shouldBeExpanded) {
                dbNodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            }

            const std::string dbNodeLabel =
                HierarchyHelpers::makeTreeNodeLabel(dbName, "db_" + dbName);

            // Force the node open every frame if it should be expanded
            // This prevents ImGui from collapsing it when content changes (e.g., tables loading)
            if (shouldBeExpanded) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

            renderMySQLDatabaseIcon();

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createSQLEditorTab("", mysqlDb, dbName);
                    std::cout << "Creating new SQL editor for database: " << dbName << std::endl;
                }
                if (ImGui::MenuItem("Show Diagram")) {
                    auto& app = Application::getInstance();
                    app.getTabManager()->createDiagramTab(mysqlDb, dbName);
                    std::cout << "Creating diagram for database: " << dbName << std::endl;
                }
                ImGui::EndPopup();
            }

            if (dbNodeOpen) {
                // Track that this database is expanded (do this immediately)
                mysqlDb->setDatabaseExpanded(dbName, true);

                // Only switch database if we're not already connected to it
                if (dbName != mysqlDb->getDatabaseName()) {
                    if (!mysqlDb->isSwitchingDatabase()) {
                        std::cout << "Starting async switch to database: " << dbName << std::endl;
                        mysqlDb->switchToDatabaseAsync(dbName);
                    }
                }

                // Show tables and views for this database (cached or load if needed)
                if (mysqlDb->isSwitchingDatabase()) {
                    HierarchyHelpers::renderLoadingState("Switching to database...",
                                                         "##switching_db_spinner");
                } else if (dbName == mysqlDb->getDatabaseName()) {
                    // Show tables for currently connected database (loading triggered inside when
                    // nodes are expanded)
                    HierarchyHelpers::renderTablesSection(mysqlDb);
                    HierarchyHelpers::renderViewsSection(mysqlDb);
                } else {
                    // Show cached tables for other databases
                    HierarchyHelpers::renderCachedTablesSection(mysqlDb, dbName);
                    HierarchyHelpers::renderCachedViewsSection(mysqlDb, dbName);
                }

                ImGui::TreePop();
            } else {
                // Node is collapsed, mark as not expanded
                if (mysqlDb->isDatabaseExpanded(dbName)) {
                    mysqlDb->setDatabaseExpanded(dbName, false);
                }
            }
        }
    }

    void renderTableNode(const std::shared_ptr<MySQLDatabase>& mysqlDb, int tableIndex) {
        HierarchyHelpers::renderTableNode(mysqlDb, tableIndex);
    }

    void renderViewNode(const std::shared_ptr<MySQLDatabase>& mysqlDb, int viewIndex) {
        HierarchyHelpers::renderViewNode(mysqlDb, viewIndex);
    }

    void renderCachedTableNode(const std::shared_ptr<MySQLDatabase>& mysqlDb,
                               const std::string& dbName, int tableIndex) {
        HierarchyHelpers::renderCachedTableNode(mysqlDb, dbName, tableIndex);
    }

    void renderCachedViewNode(const std::shared_ptr<MySQLDatabase>& mysqlDb,
                              const std::string& dbName, int viewIndex) {
        HierarchyHelpers::renderCachedViewNode(mysqlDb, dbName, viewIndex);
    }

} // namespace MySQLHierarchy
