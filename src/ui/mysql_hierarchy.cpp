#include "ui/mysql_hierarchy.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include "utils/spinner.hpp"
#include <iostream>

namespace MySQLHierarchy {
    void renderMySQLHierarchy(const std::shared_ptr<MySQLDatabase> &mysqlDb) {
        if (mysqlDb->shouldShowAllDatabases()) {
            // Show all databases from the server
            renderAllDatabasesHierarchy(mysqlDb);
        } else {
            // Show only the connected database
            renderSingleDatabaseHierarchy(mysqlDb);
        }
    }

    void renderSingleDatabaseHierarchy(const std::shared_ptr<MySQLDatabase> &mysqlDb) {
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

        // Draw database node with placeholder space for icon
        std::string dbName = actualDbName;
        // Add table count if tables are loaded
        if (mysqlDb->areTablesLoaded() && !mysqlDb->getTables().empty()) {
            dbName = std::format("{} ({} tables)", dbName, mysqlDb->getTables().size());
        }
        const std::string dbNodeLabel = std::format("   {}###db_single_{}", dbName, actualDbName);
        const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

        // Draw colored icon over the placeholder space
        const auto dbNodeIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            dbNodeIconPos,
            ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)), // Orange for MySQL database
            ICON_FK_DATABASE);

        // Context menu for database node
        if (ImGui::BeginPopupContextItem("db_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                auto &app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", mysqlDb, mysqlDb->getDatabaseName());
                std::cout << "Creating new SQL editor for database: " << mysqlDb->getDatabaseName()
                          << std::endl;
            }
            ImGui::EndPopup();
        }

        if (dbNodeOpen) {
            // Track that this database is expanded
            if (!mysqlDb->isDatabaseExpanded(actualDbName)) {
                mysqlDb->setDatabaseExpanded(actualDbName, true);
            }

            // Load tables when database node is opened
            if (!mysqlDb->areTablesLoaded() && !mysqlDb->isLoadingTables()) {
                std::cout
                    << "Database node expanded and tables not loaded yet, attempting to load..."
                    << std::endl;
                mysqlDb->refreshTables();
            }

            // Show tables and views
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

    void renderAllDatabasesHierarchy(const std::shared_ptr<MySQLDatabase> &mysqlDb) {
        // Check for async database loading completion
        if (mysqlDb->isLoadingDatabases()) {
            mysqlDb->checkDatabasesStatusAsync();
        }

        // Get all databases from the server (may trigger async loading)
        std::vector<std::string> databases = mysqlDb->getDatabaseNames();

        // Show loading indicator if databases are being loaded
        if (mysqlDb->isLoadingDatabases() && databases.empty()) {
            ImGui::Text("  Loading databases...");
            ImGui::SameLine();
            UIUtils::Spinner("##loading_databases_spinner", 6.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
            return;
        }

        if (databases.empty() && !mysqlDb->isLoadingDatabases()) {
            ImGui::Text("  No databases found");
            return;
        }

        for (const auto &dbName : databases) {
            ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_FramePadding;

            // Keep the node expanded if it was previously expanded
            if (mysqlDb->isDatabaseExpanded(dbName)) {
                dbNodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
                // Also explicitly set the next item to be open to prevent collapse
                ImGui::SetNextItemOpen(true);
            }

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
                ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)), // Orange for MySQL database
                ICON_FK_DATABASE);

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    auto &app = Application::getInstance();
                    app.getTabManager()->createSQLEditorTab("", mysqlDb, dbName);
                    std::cout << "Creating new SQL editor for database: " << dbName << std::endl;
                }
                ImGui::EndPopup();
            }

            if (dbNodeOpen) {
                // Track that this database is expanded
                if (!mysqlDb->isDatabaseExpanded(dbName)) {
                    mysqlDb->setDatabaseExpanded(dbName, true);

                    // Only switch database if we're not already connected to it
                    if (dbName != mysqlDb->getDatabaseName()) {
                        if (!mysqlDb->isSwitchingDatabase()) {
                            std::cout << "Starting async switch to database: " << dbName
                                      << std::endl;
                            mysqlDb->switchToDatabaseAsync(dbName);
                        }
                    }
                }

                // Show tables and views for this database (cached or load if needed)
                if (mysqlDb->isSwitchingDatabase()) {
                    // Show switching indicator
                    ImGui::Text("  Switching to database...");
                    ImGui::SameLine();
                    UIUtils::Spinner("##switching_db_spinner", 6.0f, 2,
                                     ImGui::GetColorU32(ImGuiCol_Text));
                } else if (dbName == mysqlDb->getDatabaseName()) {
                    // Load tables only when first expanded and not already loaded
                    if (!mysqlDb->areTablesLoaded() && !mysqlDb->isLoadingTables()) {
                        std::cout << "Database " << dbName
                                  << " expanded for first time, loading tables..." << std::endl;
                        mysqlDb->refreshTables();
                    }
                    // Show tables for currently connected database
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

    void renderTableNode(const std::shared_ptr<MySQLDatabase> &mysqlDb, int tableIndex) {
        HierarchyHelpers::renderTableNode(mysqlDb, tableIndex);
    }

    void renderViewNode(const std::shared_ptr<MySQLDatabase> &mysqlDb, int viewIndex) {
        HierarchyHelpers::renderViewNode(mysqlDb, viewIndex);
    }

    void renderCachedTableNode(const std::shared_ptr<MySQLDatabase> &mysqlDb,
                               const std::string &dbName, int tableIndex) {
        HierarchyHelpers::renderCachedTableNode(mysqlDb, dbName, tableIndex);
    }

    void renderCachedViewNode(const std::shared_ptr<MySQLDatabase> &mysqlDb,
                              const std::string &dbName, int viewIndex) {
        HierarchyHelpers::renderCachedViewNode(mysqlDb, dbName, viewIndex);
    }

} // namespace MySQLHierarchy
