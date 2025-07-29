#include "ui/mysql_hierarchy.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
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
        // Show the database/schema as a child node under the connection
        constexpr ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_FramePadding;

        // Draw database node with placeholder space for icon
        const std::string dbName = mysqlDb->getDatabaseName();
        const std::string dbNodeLabel = std::format("   {}###db_node", dbName);
        const bool dbNodeOpen = ImGui::TreeNodeEx(dbNodeLabel.c_str(), dbNodeFlags);

        // Draw colored icon over the placeholder space
        const auto dbNodeIconPos =
            ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                   ImGui::GetItemRectMin().y +
                       (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(
            dbNodeIconPos, ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)), // orange
            ICON_FK_DATABASE);

        // Context menu for database node
        if (ImGui::BeginPopupContextItem("db_context_menu")) {
            if (ImGui::MenuItem("New SQL Editor")) {
                const auto &app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", mysqlDb->getConnectionString());
                std::cout << "Creating new SQL editor for database: " << mysqlDb->getDatabaseName()
                          << std::endl;
            }
            ImGui::EndPopup();
        }

        if (dbNodeOpen) {
            // Show tables and views under the database node
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
                dbNodeIconPos, ImGui::GetColorU32(ImVec4(1.0f, 0.6f, 0.2f, 1.0f)), // orange
                ICON_FK_DATABASE);

            // Context menu for database node
            if (ImGui::BeginPopupContextItem(("db_context_menu_" + dbName).c_str())) {
                if (ImGui::MenuItem("New SQL Editor")) {
                    const auto &app = Application::getInstance();
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
                                dbName, false); // Mark as not expanded due to failure
                            ImGui::TreePop();
                            continue;
                        }
                    }
                }

                // Only show tables/views if we're currently connected to this database
                if (dbName == mysqlDb->getDatabaseName()) {
                    // Show tables and views for this database (same as single database mode)
                    HierarchyHelpers::renderTablesSection(mysqlDb);
                    HierarchyHelpers::renderViewsSection(mysqlDb);
                } else {
                    // We're showing a different database, so show placeholder
                    ImGui::Text("  Switch to this database to view contents");
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
} // namespace MySQLHierarchy
