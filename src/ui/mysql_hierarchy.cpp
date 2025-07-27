#include "ui/mysql_hierarchy.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "imgui.h"
#include "ui/hierarchy_helpers.hpp"
#include <iostream>

namespace MySQLHierarchy {
    void renderMySQLHierarchy(MySQLDatabase *mysqlDb) {
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
} // namespace MySQLHierarchy
