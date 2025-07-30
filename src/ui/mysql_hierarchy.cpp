#include "ui/mysql_hierarchy.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include "utils/spinner.hpp"
#include <iostream>

namespace MySQLHierarchy {
    void renderMySQLHierarchy(MySQLDatabase *mysqlDb) {
        if (mysqlDb->shouldShowAllDatabases()) {
            // Show all databases from the server
            renderAllDatabasesHierarchy(mysqlDb);
        } else {
            // Show only the connected database
            renderSingleDatabaseHierarchy(mysqlDb);
        }
    }

    void renderSingleDatabaseHierarchy(MySQLDatabase *mysqlDb) {
        // First show the connected database as a child node
        constexpr ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        // Draw database node with placeholder space for icon
        std::string dbName = mysqlDb->getDatabaseName();
        // Add table count if tables are loaded
        if (mysqlDb->areTablesLoaded() && !mysqlDb->getTables().empty()) {
            dbName = std::format("{} ({} tables)", dbName, mysqlDb->getTables().size());
        }
        const std::string dbNodeLabel = std::format(
            "   {}###db_single_{}", dbName, dbName); // 3 spaces for icon, unique ID per database
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
                app.getTabManager()->createSQLEditorTab("", mysqlDb->getConnectionString());
                std::cout << "Creating new SQL editor for database: " << mysqlDb->getDatabaseName()
                          << std::endl;
            }
            ImGui::EndPopup();
        }

        if (dbNodeOpen) {
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
        }
    }

    void renderAllDatabasesHierarchy(MySQLDatabase *mysqlDb) {
        // Load databases only once
        if (!mysqlDb->areDatabasesLoaded()) {
            mysqlDb->getDatabaseNames(); // This will load and cache the databases
        }

        // Get all databases from the server (cached)
        std::vector<std::string> databases = mysqlDb->getDatabaseNames();

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
                ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)), // Orange for MySQL database
                ICON_FK_DATABASE);

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    auto &app = Application::getInstance();
                    app.getTabManager()->createSQLEditorTab("", mysqlDb->getConnectionString());
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
                        std::cout << "Switching to database: " << dbName << std::endl;
                        auto [success, error] = mysqlDb->switchToDatabase(dbName);
                        if (!success) {
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                               "  Failed to connect to database:");
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  %s",
                                               error.c_str());
                            mysqlDb->setDatabaseExpanded(
                                dbName,
                                false); // Mark as not expanded due to failure
                            ImGui::TreePop();
                            continue;
                        }
                    }
                }

                // Show tables and views for this database (cached or load if needed)
                if (dbName == mysqlDb->getDatabaseName()) {
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

    void renderTableNode(MySQLDatabase *mysqlDb, int tableIndex) {
        HierarchyHelpers::renderTableNode(mysqlDb, tableIndex);
    }

    void renderViewNode(MySQLDatabase *mysqlDb, int viewIndex) {
        HierarchyHelpers::renderViewNode(mysqlDb, viewIndex);
    }

    void renderCachedTableNode(MySQLDatabase *mysqlDb, const std::string &dbName, int tableIndex) {
        HierarchyHelpers::renderCachedTableNode(mysqlDb, dbName, tableIndex);
    }

    void renderCachedViewNode(MySQLDatabase *mysqlDb, const std::string &dbName, int viewIndex) {
        HierarchyHelpers::renderCachedViewNode(mysqlDb, dbName, viewIndex);
    }

} // namespace MySQLHierarchy
