#include "ui/tab/table_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/sql_builder.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>

namespace {
    ImVec4 withAlpha(ImVec4 color, const float alpha) {
        color.w = alpha;
        return color;
    }

    ImVec4 blend(ImVec4 a, const ImVec4& b, const float amount) {
        a.x += (b.x - a.x) * amount;
        a.y += (b.y - a.y) * amount;
        a.z += (b.z - a.z) * amount;
        a.w += (b.w - a.w) * amount;
        return a;
    }

    void renderStateChip(const Theme::Colors& colors, const char* label, const ImVec4& tint) {
        const ImVec2 padding(Theme::Spacing::M, Theme::Spacing::S);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        auto* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(
            pos, ImVec2(pos.x + size.x, pos.y + size.y),
            ImGui::GetColorU32(withAlpha(blend(colors.surface0, tint, 0.18f), 0.95f)), 0.0f);
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          ImGui::GetColorU32(withAlpha(blend(colors.overlay0, tint, 0.28f), 0.95f)),
                          0.0f, 0, 1.0f);
        drawList->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), ImGui::GetColorU32(tint),
                          label);
        ImGui::Dummy(size);
    }

    void renderSectionTitle(const Theme::Colors& colors, const ImVec4& accent, const char* icon,
                            const char* title, const char* subtitle = nullptr) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        auto* drawList = ImGui::GetWindowDrawList();

        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Theme::Spacing::M);

        ImGui::PushStyleColor(ImGuiCol_Text, colors.text);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        const float headerBottom = ImGui::GetItemRectMax().y + Theme::Spacing::S;
        drawList->AddLine(
            ImVec2(start.x, headerBottom),
            ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, headerBottom),
            ImGui::GetColorU32(withAlpha(blend(colors.overlay0, accent, 0.2f), 0.8f)), 1.0f);

        if (subtitle && subtitle[0] != '\0') {
            ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::XS));
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextWrapped("%s", subtitle);
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
    }

    void renderNoticeBox(const Theme::Colors& colors, const char* id, const char* icon,
                         const char* message, const ImVec4& accent) {
        const ImVec2 padding(Theme::Spacing::M, Theme::Spacing::M);
        const float iconWidth = ImGui::CalcTextSize(icon).x;
        const float wrapWidth =
            std::max(40.0f, ImGui::GetContentRegionAvail().x - padding.x * 2.0f - iconWidth -
                                Theme::Spacing::M);
        const ImVec2 textSize = ImGui::CalcTextSize(message, nullptr, false, wrapWidth);
        const float height = std::max(textSize.y, ImGui::GetTextLineHeight()) + padding.y * 2.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              withAlpha(blend(colors.surface0, accent, 0.08f), 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border,
                              withAlpha(blend(colors.overlay0, accent, 0.3f), 0.95f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::BeginChild(id, ImVec2(0, height), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Theme::Spacing::M);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
        ImGui::TextWrapped("%s", message);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
} // namespace

TableEditorTab::TableEditorTab(IDatabaseNode* node, const std::string& schema)
    : Tab("Create Table", TabType::TABLE_EDITOR) {
    reset();
    editorMode = TableEditorMode::Create;
    dbNode = node;
    if (node) {
        databaseType = node->getDatabaseType();
    }
    schemaName = schema;
    editingTable = Table{};
    editingTable.name = "New Table";
    std::strncpy(tableNameBuffer, "New Table", sizeof(tableNameBuffer) - 1);
    editingTable.columns.clear();
    rightPanelMode = RightPanelMode::TableProperties;
    initializeColumnTypeAutoComplete();
}

TableEditorTab::TableEditorTab(IDatabaseNode* node, const Table& table, const std::string& schema)
    : Tab("Edit: " + table.name, TabType::TABLE_EDITOR) {
    reset();
    editorMode = TableEditorMode::Edit;
    dbNode = node;
    databaseType = node ? node->getDatabaseType() : DatabaseType::SQLITE;
    schemaName = schema;
    editingTable = table;
    originalTable = table;
    std::strncpy(tableNameBuffer, table.name.c_str(), sizeof(tableNameBuffer) - 1);
    tableNameBuffer[sizeof(tableNameBuffer) - 1] = '\0';
    std::memset(tableCommentBuffer, 0, sizeof(tableCommentBuffer));
    rightPanelMode = RightPanelMode::TableProperties;
    initializeColumnTypeAutoComplete();
}

void TableEditorTab::render() {
    bool closeRequested = false;
    renderContent(closeRequested);
    if (closeRequested) {
        requestClose();
    }
}

void TableEditorTab::renderContent(bool& closeRequested) {
    const auto& colors = Application::getInstance().getCurrentColors();

    constexpr float saveButtonWidth = 120.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, withAlpha(colors.surface1, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, withAlpha(colors.surface2, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(colors.overlay0, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Table: ");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - saveButtonWidth - Theme::Spacing::M);
    if (ImGui::InputText("##table_name_header", tableNameBuffer, sizeof(tableNameBuffer))) {
        editingTable.name = tableNameBuffer;
        markDirty();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::SameLine(0, Theme::Spacing::M);
    renderButtons(closeRequested);
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(Theme::Spacing::M + 2.0f, Theme::Spacing::S + 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 18.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(colors.overlay0, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator, withAlpha(colors.overlay0, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_Header,
                          withAlpha(blend(colors.surface0, colors.blue, 0.08f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          withAlpha(blend(colors.surface1, colors.blue, 0.18f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          withAlpha(blend(colors.surface2, colors.blue, 0.24f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, withAlpha(colors.blue, 0.22f));

    constexpr float splitterWidth = 6.0f;
    constexpr float minLeftPanelWidth = 220.0f;
    constexpr float minRightPanelWidth = 320.0f;
    const ImVec2 panelRegion = ImGui::GetContentRegionAvail();
    const float panelHeight = std::max(220.0f, panelRegion.y);
    const float maxLeftPanelWidth =
        std::max(minLeftPanelWidth, panelRegion.x - minRightPanelWidth - splitterWidth);
    leftPanelWidth = std::clamp(leftPanelWidth, minLeftPanelWidth, maxLeftPanelWidth);
    const float rightPanelWidth =
        std::max(minRightPanelWidth, panelRegion.x - leftPanelWidth - splitterWidth);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::M));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, withAlpha(colors.mantle, 0.72f));
    ImGui::BeginChild("LeftPanel", ImVec2(leftPanelWidth, panelHeight), true);
    renderLeftPanel();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);

    const ImVec2 splitterPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##table_editor_splitter", ImVec2(splitterWidth, panelHeight));
    const bool splitterHovered = ImGui::IsItemHovered();
    const bool splitterHeld = ImGui::IsItemActive();
    if (splitterHovered || splitterHeld) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (splitterHeld && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        leftPanelWidth += ImGui::GetIO().MouseDelta.x;
        leftPanelWidth = std::clamp(leftPanelWidth, minLeftPanelWidth, maxLeftPanelWidth);
    }
    ImGui::GetWindowDrawList()->AddRectFilled(
        splitterPos, ImVec2(splitterPos.x + splitterWidth, splitterPos.y + panelHeight),
        ImGui::GetColorU32(withAlpha(splitterHeld      ? blend(colors.surface2, colors.blue, 0.2f)
                                     : splitterHovered ? blend(colors.surface1, colors.blue, 0.14f)
                                                       : colors.mantle,
                                     1.0f)),
        0.0f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(splitterPos.x + splitterWidth * 0.5f, splitterPos.y),
        ImVec2(splitterPos.x + splitterWidth * 0.5f, splitterPos.y + panelHeight),
        ImGui::GetColorU32(
            withAlpha(splitterHeld || splitterHovered ? colors.blue : colors.overlay0,
                      splitterHeld || splitterHovered ? 0.9f : 0.65f)),
        1.0f);

    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::M));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, withAlpha(colors.surface0, 0.82f));
    ImGui::BeginChild("RightPanel", ImVec2(rightPanelWidth, panelHeight), true);
    renderRightPanel();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (!errorMessage.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("Error: %s", errorMessage.c_str());
        ImGui::PopStyleColor();
    }

    renderPreviewPopup(closeRequested);

    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(8);
}

void TableEditorTab::renderLeftPanel() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::Indent(Theme::Spacing::M);
    ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    renderSectionTitle(colors, colors.green, ICON_FA_SITEMAP, "Table Structure",
                       "Navigate the table tree and select a node to edit.");
    ImGui::PopStyleColor();
    renderTableTree();
    ImGui::Unindent(Theme::Spacing::M);
}

void TableEditorTab::renderRightPanel() {
    ImGui::Indent(Theme::Spacing::M);
    switch (rightPanelMode) {
    case RightPanelMode::TableProperties:
        renderTableProperties();
        break;
    case RightPanelMode::ColumnEditor:
        renderColumnEditor();
        break;
    case RightPanelMode::Instructions:
    default:
        renderInstructions();
        break;
    }
    ImGui::Unindent(Theme::Spacing::M);
}

void TableEditorTab::renderTableTree() {
    renderColumnsNode();
    renderKeysNode();
}

void TableEditorTab::renderColumnsNode() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGuiTreeNodeFlags columnsFlags = ImGuiTreeNodeFlags_DefaultOpen |
                                      ImGuiTreeNodeFlags_OpenOnArrow |
                                      ImGuiTreeNodeFlags_FramePadding;

    const std::string columnsLabel =
        std::format("   Columns ({})      ", editingTable.columns.size());
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    const bool columnsOpen = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);
    ImGui::PopStyleColor(3);

    const auto columnsIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(columnsIconPos, ImGui::GetColorU32(colors.green),
                                        ICON_FA_TABLE_COLUMNS);

    const auto plusIconPos =
        ImVec2(ImGui::GetItemRectMax().x - 25.0f,
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    const float plusIconCenterY = plusIconPos.y + ImGui::GetTextLineHeight() * 0.5f;
    const ImVec2 plusIconMin = ImVec2(plusIconPos.x - 5.0f, plusIconCenterY - 10.0f);
    const ImVec2 plusIconMax = ImVec2(plusIconPos.x + 15.0f, plusIconCenterY + 10.0f);
    const bool isPlusHovered = ImGui::IsMouseHoveringRect(plusIconMin, plusIconMax);
    auto* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(
        plusIconMin, plusIconMax,
        ImGui::GetColorU32(withAlpha(isPlusHovered ? blend(colors.surface1, colors.green, 0.18f)
                                                   : blend(colors.surface0, colors.overlay0, 0.1f),
                                     0.95f)),
        0.0f);
    drawList->AddRect(
        plusIconMin, plusIconMax,
        ImGui::GetColorU32(withAlpha(isPlusHovered ? blend(colors.green, colors.text, 0.15f)
                                                   : blend(colors.overlay0, colors.green, 0.12f),
                                     0.9f)),
        0.0f, 0, 1.0f);

    const ImU32 plusColor = isPlusHovered ? ImGui::GetColorU32(colors.green)
                                          : ImGui::GetColorU32(withAlpha(colors.overlay1, 0.9f));
    drawList->AddText(plusIconPos, plusColor, ICON_FA_PLUS);

    if (isPlusHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        startAddColumn();
        rightPanelMode = RightPanelMode::ColumnEditor;
    }

    if (isPlusHovered) {
        ImGui::SetTooltip("Add New Column");
    }

    if (ImGui::BeginPopupContextItem("columns_context_menu")) {
        if (ImGui::MenuItem("Add New Column")) {
            startAddColumn();
            rightPanelMode = RightPanelMode::ColumnEditor;
        }
        ImGui::EndPopup();
    }

    if (columnsOpen) {
        for (size_t i = 0; i < editingTable.columns.size(); i++) {
            const auto& column = editingTable.columns[i];

            ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

            if (selectedColumnIndex == static_cast<int>(i)) {
                columnFlags |= ImGuiTreeNodeFlags_Selected;
            }

            std::string columnDisplay = std::format("{} ({})", column.name, column.type);
            if (column.isPrimaryKey) {
                columnDisplay += ", PK";
            }
            if (column.isNotNull) {
                columnDisplay += ", NOT NULL";
            }
            if (column.isUnique) {
                columnDisplay += ", UNIQUE";
            }
            if (column.isAutoIncrement) {
                columnDisplay += ", AI";
            }

            ImGui::PushID(static_cast<int>(i));
            ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);

            if (ImGui::IsItemClicked()) {
                startEditColumn(static_cast<int>(i));
                rightPanelMode = RightPanelMode::ColumnEditor;
            }

            if (ImGui::BeginPopupContextItem("column_context_menu")) {
                if (ImGui::MenuItem("Edit Column")) {
                    startEditColumn(static_cast<int>(i));
                    rightPanelMode = RightPanelMode::ColumnEditor;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Column")) {
                    editingTable.columns.erase(editingTable.columns.begin() + static_cast<long>(i));
                    if (selectedColumnIndex == static_cast<int>(i)) {
                        rightPanelMode = RightPanelMode::TableProperties;
                        columnEditMode = ColumnEditMode::None;
                        selectedColumnIndex = -1;
                        resetColumnForm();
                    } else if (selectedColumnIndex > static_cast<int>(i)) {
                        selectedColumnIndex--;
                    }
                    markDirty();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void TableEditorTab::renderKeysNode() const {
    const auto& colors = Application::getInstance().getCurrentColors();
    constexpr ImGuiTreeNodeFlags keysFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string keysLabel = "   Keys";
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    const bool keysOpen = ImGui::TreeNodeEx(keysLabel.c_str(), keysFlags);
    ImGui::PopStyleColor(3);

    const auto keysIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(keysIconPos, ImGui::GetColorU32(colors.yellow),
                                        ICON_FA_KEY);

    if (keysOpen) {
        bool hasPrimaryKey = false;
        std::string primaryKeyColumns;
        for (const auto& column : editingTable.columns) {
            if (column.isPrimaryKey) {
                if (hasPrimaryKey) {
                    primaryKeyColumns += ", ";
                }
                primaryKeyColumns += column.name;
                hasPrimaryKey = true;
            }
        }

        if (hasPrimaryKey) {
            const ImGuiTreeNodeFlags pkFlags = ImGuiTreeNodeFlags_Leaf |
                                               ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                               ImGuiTreeNodeFlags_FramePadding;
            const std::string pkDisplay = "Primary Key (" + primaryKeyColumns + ")";
            ImGui::TreeNodeEx(pkDisplay.c_str(), pkFlags);
        } else {
            ImGui::Text("  No primary key");
        }
        ImGui::TreePop();
    }
}

void TableEditorTab::renderColumnEditor() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    const char* editorTitle =
        (columnEditMode == ColumnEditMode::Add) ? "Add New Column" : "Edit Column";
    renderSectionTitle(colors, colors.blue, ICON_FA_PEN_TO_SQUARE, editorTitle,
                       "Adjust the selected column definition and constraints.");

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(colors.overlay0, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(colors.surface0, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, withAlpha(colors.surface1, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, withAlpha(colors.surface2, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, colors.blue);

    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::Text("Column Name:");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-Theme::Spacing::M);
    if (focusColumnName) {
        ImGui::SetKeyboardFocusHere();
        focusColumnName = false;
    }
    if (ImGui::InputText("##column_name", columnName, sizeof(columnName))) {
        updateCurrentColumn();
    }

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::Text("Data Type:");
    ImGui::PopStyleColor();
    if (columnTypeAutoComplete == nullptr) {
        initializeColumnTypeAutoComplete();
    }
    columnTypeAutoComplete->render("##column_type", columnType, sizeof(columnType));

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::Text("Constraints:");
    ImGui::PopStyleColor();
    if (ImGui::Checkbox("Primary Key", &isPrimaryKey)) {
        updateCurrentColumn();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("NOT NULL", &isNotNull)) {
        updateCurrentColumn();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("UNIQUE", &isUnique)) {
        updateCurrentColumn();
    }

    {
        const char* label = nullptr;
        const char* tooltip = nullptr;
        switch (databaseType) {
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB:
        case DatabaseType::SQLITE:
            label = "AUTO_INCREMENT";
            break;
        case DatabaseType::POSTGRESQL:
            label = "SERIAL";
            tooltip = "Uses SERIAL/BIGSERIAL type for auto-incrementing columns";
            break;
        case DatabaseType::MSSQL:
            label = "IDENTITY";
            tooltip = "IDENTITY(1,1)";
            break;
        case DatabaseType::ORACLE:
            label = "IDENTITY";
            tooltip = "GENERATED ALWAYS AS IDENTITY";
            break;
        default:
            break;
        }
        if (label) {
            const bool typeSupported = supportsAutoIncrement(databaseType, columnType);
            // clear flag if the type was changed to something incompatible
            if (!typeSupported && isAutoIncrement) {
                isAutoIncrement = false;
                updateCurrentColumn();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!typeSupported);
            if (ImGui::Checkbox(label, &isAutoIncrement)) {
                updateCurrentColumn();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!typeSupported) {
                    if (databaseType == DatabaseType::SQLITE) {
                        ImGui::SetTooltip("Only INTEGER PRIMARY KEY supports AUTOINCREMENT");
                    } else {
                        ImGui::SetTooltip("Only integer types support auto-increment");
                    }
                } else if (tooltip) {
                    ImGui::SetTooltip("%s", tooltip);
                }
            }
        }
    }

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::Text("Default Value (optional):");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-Theme::Spacing::M);
    if (ImGui::InputText("##default_value", defaultValue, sizeof(defaultValue))) {
        updateCurrentColumn();
    }

    ImGui::Spacing();

    if (databaseType == DatabaseType::MYSQL || databaseType == DatabaseType::MARIADB ||
        databaseType == DatabaseType::POSTGRESQL) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
        ImGui::Text("Comment:");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##column_comment", columnComment, sizeof(columnComment),
                                      ImVec2(-Theme::Spacing::M, 60))) {
            updateCurrentColumn();
        }
    }

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();

    ImGui::Spacing();
}

void TableEditorTab::renderPreviewPopup(bool& closeRequested) {
    const auto& colors = Application::getInstance().getCurrentColors();

    if (showPreviewPopup) {
        ImGui::OpenPopup("SQL Preview###sql_preview_popup");
        showPreviewPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700, 460), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("SQL Preview###sql_preview_popup", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::TextUnformatted("Review the SQL below, then click Execute to apply the changes.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const float buttonsHeight =
        ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::M * 2.0f + Theme::Spacing::S;
    previewEditor.SetPalette(dearsql::TextEditor::FromTheme(colors));
    previewEditor.Render("##preview_sql",
                         ImVec2(0, ImGui::GetContentRegionAvail().y - buttonsHeight), true);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        ImVec4(colors.green.x * 0.8f, colors.green.y * 0.8f, colors.green.z * 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.base);

    if (ImGui::Button("Execute", ImVec2(120, 0))) {
        if (dbNode) {
            if (editorMode == TableEditorMode::Create) {
                Table resultTable = buildResultTable();
                spdlog::debug("Creating table: {}", resultTable.name);
                auto [success, error] = dbNode->createTable(resultTable);
                if (success) {
                    dbNode->startTablesLoadAsync(true);
                } else {
                    errorMessage = error;
                }
            } else {
                const auto statements = generateAlterTableStatements();
                for (const auto& sql : statements) {
                    spdlog::debug("Executing: {}", sql);
                    auto result = dbNode->executeQuery(sql);
                    if (!result.success()) {
                        errorMessage = result.errorMessage();
                        break;
                    }
                }
                if (errorMessage.empty()) {
                    dbNode->startTablesLoadAsync(true);
                }
            }
        }

        if (errorMessage.empty()) {
            dirty = false;
            ImGui::CloseCurrentPopup();
            closeRequested = true;
        }
    }

    ImGui::PopStyleColor(5);
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.overlay1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay2);

    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

void TableEditorTab::renderButtons(bool& closeRequested) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        ImVec4(colors.green.x * 0.8f, colors.green.y * 0.8f, colors.green.z * 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.base);

    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save", ImVec2(120, 0))) {
        if (validateTableInput()) {
            errorMessage.clear();
            std::string sql;
            if (editorMode == TableEditorMode::Create) {
                sql = generateCreateTableSQL();
            } else {
                const auto statements = generateAlterTableStatements();
                for (size_t i = 0; i < statements.size(); ++i) {
                    if (i > 0)
                        sql += ";\n";
                    sql += statements[i];
                }
            }
            previewEditor.SetText(sql);
            showPreviewPopup = true;
        }
    }

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
}

void TableEditorTab::startAddColumn() {
    Column newColumn;
    newColumn.name = "column_name";
    newColumn.type = getCommonDataTypes().front();
    newColumn.comment.clear();
    newColumn.defaultValue.clear();
    newColumn.isPrimaryKey = false;
    newColumn.isNotNull = false;
    newColumn.isUnique = false;
    newColumn.isAutoIncrement = false;

    editingTable.columns.push_back(newColumn);
    columnEditMode = ColumnEditMode::Add;
    selectedColumnIndex = static_cast<int>(editingTable.columns.size() - 1);
    rightPanelMode = RightPanelMode::ColumnEditor;
    populateColumnFormFromColumn(newColumn);
    focusColumnName = true;
    errorMessage.clear();
    markDirty();
}

void TableEditorTab::startEditColumn(const int columnIndex) {
    if (columnIndex >= 0 && columnIndex < static_cast<int>(editingTable.columns.size())) {
        columnEditMode = ColumnEditMode::Edit;
        selectedColumnIndex = columnIndex;
        rightPanelMode = RightPanelMode::ColumnEditor;
        originalColumnName = editingTable.columns[columnIndex].name;
        populateColumnFormFromColumn(editingTable.columns[columnIndex]);
        errorMessage.clear();
    }
}

void TableEditorTab::cancelColumnEdit() {
    columnEditMode = ColumnEditMode::None;
    selectedColumnIndex = -1;
    rightPanelMode = RightPanelMode::TableProperties;
    resetColumnForm();
    errorMessage.clear();
}

void TableEditorTab::resetColumnForm() {
    std::memset(columnName, 0, sizeof(columnName));
    std::memset(columnType, 0, sizeof(columnType));
    std::memset(columnComment, 0, sizeof(columnComment));
    std::memset(defaultValue, 0, sizeof(defaultValue));
    isPrimaryKey = false;
    isNotNull = false;
    isUnique = false;
    isAutoIncrement = false;
    if (columnTypeAutoComplete) {
        columnTypeAutoComplete->hideAutoComplete();
    }
}

void TableEditorTab::populateColumnFormFromColumn(const Column& column) {
    std::strncpy(columnName, column.name.c_str(), sizeof(columnName) - 1);
    columnName[sizeof(columnName) - 1] = '\0';
    std::strncpy(columnType, column.type.c_str(), sizeof(columnType) - 1);
    columnType[sizeof(columnType) - 1] = '\0';
    std::strncpy(columnComment, column.comment.c_str(), sizeof(columnComment) - 1);
    columnComment[sizeof(columnComment) - 1] = '\0';
    std::strncpy(defaultValue, column.defaultValue.c_str(), sizeof(defaultValue) - 1);
    defaultValue[sizeof(defaultValue) - 1] = '\0';
    isPrimaryKey = column.isPrimaryKey;
    isNotNull = column.isNotNull;
    isUnique = column.isUnique;
    isAutoIncrement = column.isAutoIncrement;
    if (columnTypeAutoComplete) {
        columnTypeAutoComplete->hideAutoComplete();
    }
}

void TableEditorTab::initializeColumnTypeAutoComplete() {
    AutoCompleteInput::Config config;
    config.width = -Theme::Spacing::M;
    config.keywords = getCommonDataTypes();
    config.matchMode = AutoCompleteInput::MatchMode::Substring;
    config.completionMode = AutoCompleteInput::CompletionMode::WholeInput;
    config.appendSpaceOnComplete = false;
    config.showSuggestionsOnEmpty = true;
    config.maxSuggestionsShown = 6;
    config.refocusOnComplete = false;
    config.onChange = [this]() { updateCurrentColumn(); };
    columnTypeAutoComplete = std::make_unique<AutoCompleteInput>(std::move(config));
}

std::string TableEditorTab::generateAddColumnSQL() const {
    std::string tableName = std::strlen(tableNameBuffer) > 0 ? tableNameBuffer : originalTable.name;

    std::string qualifiedTableName = tableName;
    if (databaseType == DatabaseType::POSTGRESQL) {
        if (qualifiedTableName.find('.') == std::string::npos) {
            const std::string schema = schemaName.empty() ? "public" : schemaName;
            qualifiedTableName = schema + "." + qualifiedTableName;
        }
    }

    std::string colType = std::string(columnType);
    if (isAutoIncrement && databaseType == DatabaseType::POSTGRESQL) {
        std::string lower = colType;
        std::ranges::transform(lower, lower.begin(), ::tolower);
        if (lower == "bigint")
            colType = "BIGSERIAL";
        else if (lower == "smallint")
            colType = "SMALLSERIAL";
        else
            colType = "SERIAL";
    }

    std::string sql = "ALTER TABLE " + qualifiedTableName + " ADD COLUMN " +
                      std::string(columnName) + " " + colType;

    if (isNotNull) {
        sql += " NOT NULL";
    }
    if (isAutoIncrement && databaseType != DatabaseType::POSTGRESQL) {
        sql += autoIncrementClause(databaseType);
    }
    if (isUnique) {
        sql += " UNIQUE";
    }
    if (std::strlen(defaultValue) > 0) {
        sql += " DEFAULT " + std::string(defaultValue);
    }
    if (std::strlen(columnComment) > 0) {
        if (databaseType == DatabaseType::MYSQL || databaseType == DatabaseType::MARIADB) {
            sql += " COMMENT '" + std::string(columnComment) + "'";
        } else if (databaseType == DatabaseType::POSTGRESQL) {
            sql += "; COMMENT ON COLUMN " + qualifiedTableName + "." + std::string(columnName) +
                   " IS '" + std::string(columnComment) + "'";
        }
    }

    return sql;
}

std::string TableEditorTab::generateEditColumnSQL() const {
    std::string tableName = std::strlen(tableNameBuffer) > 0 ? tableNameBuffer : originalTable.name;
    std::string sql;

    switch (databaseType) {
    case DatabaseType::POSTGRESQL: {
        std::string qualifiedTableName = tableName;
        if (qualifiedTableName.find('.') == std::string::npos) {
            const std::string schema = schemaName.empty() ? "public" : schemaName;
            qualifiedTableName = schema + "." + qualifiedTableName;
        }

        std::vector<std::string> statements;
        if (std::string(columnName) != originalColumnName) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " RENAME COLUMN " +
                                 originalColumnName + " TO " + std::string(columnName));
        }
        statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                             std::string(columnName) + " TYPE " + std::string(columnType));

        if (isNotNull) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " SET NOT NULL");
        } else {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " DROP NOT NULL");
        }

        if (std::strlen(defaultValue) > 0) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " SET DEFAULT " +
                                 std::string(defaultValue));
        } else {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " DROP DEFAULT");
        }

        if (std::strlen(columnComment) > 0) {
            statements.push_back("COMMENT ON COLUMN " + qualifiedTableName + "." +
                                 std::string(columnName) + " IS '" + std::string(columnComment) +
                                 "'");
        }

        for (size_t i = 0; i < statements.size(); ++i) {
            if (i > 0) {
                sql += "; ";
            }
            sql += statements[i];
        }
        break;
    }

    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        sql = "ALTER TABLE " + tableName + " MODIFY COLUMN " + std::string(columnName) + " " +
              std::string(columnType);
        if (isNotNull) {
            sql += " NOT NULL";
        }
        if (isAutoIncrement) {
            sql += autoIncrementClause(databaseType);
        }
        if (std::strlen(defaultValue) > 0) {
            sql += " DEFAULT " + std::string(defaultValue);
        }
        if (std::strlen(columnComment) > 0) {
            sql += " COMMENT '" + std::string(columnComment) + "'";
        }
        break;

    case DatabaseType::MSSQL:
        sql = "ALTER TABLE " + tableName + " ALTER COLUMN " + std::string(columnName) + " " +
              std::string(columnType);
        if (isNotNull) {
            sql += " NOT NULL";
        }
        break;

    case DatabaseType::ORACLE:
        sql = "ALTER TABLE " + tableName + " MODIFY " + std::string(columnName) + " " +
              std::string(columnType);
        if (isNotNull) {
            sql += " NOT NULL";
        }
        break;

    case DatabaseType::SQLITE:
        sql = "-- SQLite doesn't support column modification directly";
        break;

    default:
        sql = "-- Column editing not supported for this database type";
        break;
    }

    return sql;
}

std::vector<std::string> TableEditorTab::getCommonDataTypes() const {
    std::vector<std::string> types;

    switch (databaseType) {
    case DatabaseType::POSTGRESQL:
        types = {
            "INTEGER",      "BIGINT", "SMALLINT", "DECIMAL", "NUMERIC", "REAL", "DOUBLE PRECISION",
            "VARCHAR(255)", "TEXT",   "CHAR(10)", "BOOLEAN", "DATE",    "TIME", "TIMESTAMP",
            "UUID",         "JSON",   "JSONB"};
        break;

    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        types = {"INT",    "BIGINT",       "SMALLINT",  "TINYINT",  "DECIMAL(10,2)", "FLOAT",
                 "DOUBLE", "VARCHAR(255)", "TEXT",      "CHAR(10)", "BOOLEAN",       "DATE",
                 "TIME",   "DATETIME",     "TIMESTAMP", "JSON"};
        break;

    case DatabaseType::SQLITE:
        types = {"INTEGER", "REAL", "TEXT", "BLOB", "NUMERIC"};
        break;

    default:
        types = {"INTEGER", "VARCHAR(255)", "TEXT", "BOOLEAN", "DATE"};
        break;
    }

    return types;
}

void TableEditorTab::renderTableProperties() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));
    renderSectionTitle(colors, colors.teal, ICON_FA_SLIDERS, "Table Properties",
                       "Manage table identity and high-level options.");

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(colors.overlay0, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(colors.surface0, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, withAlpha(colors.surface1, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, withAlpha(colors.surface2, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, colors.blue);

    if (databaseType == DatabaseType::MYSQL || databaseType == DatabaseType::MARIADB ||
        databaseType == DatabaseType::POSTGRESQL) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
        ImGui::Text("Comment:");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##table_comment", tableCommentBuffer,
                                      sizeof(tableCommentBuffer), ImVec2(-Theme::Spacing::M, 60))) {
            markDirty();
        }
        ImGui::Spacing();
    }

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::Spacing();

    if (editorMode == TableEditorMode::Create) {
        if (editingTable.columns.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "Note: A table must have at least one column to be created.");
        }
    } else {
        renderNoticeBox(colors, "edit_table_note", ICON_FA_CIRCLE_INFO,
                        "Use the structure tree to add, remove, or select columns without leaving "
                        "this editor.",
                        colors.teal);
    }
}

void TableEditorTab::renderInstructions() {
    const auto& colors = Application::getInstance().getCurrentColors();
    renderSectionTitle(colors, colors.yellow, ICON_FA_CIRCLE_INFO, "Table Editor",
                       "Click the table node to edit properties, or select a column to edit it. "
                       "Right-click on 'Columns' to add a new column.");
    renderNoticeBox(colors, "table_editor_intro", ICON_FA_ARROW_POINTER,
                    "The left side is the structure tree. The right side updates based on the node "
                    "you select.",
                    colors.yellow);
}

bool TableEditorTab::validateTableInput() {
    errorMessage.clear();

    if (std::strlen(tableNameBuffer) == 0) {
        errorMessage = "Table name cannot be empty";
        return false;
    }

    if (editingTable.columns.empty()) {
        errorMessage = "Table must have at least one column";
        return false;
    }

    for (const auto& col : editingTable.columns) {
        if (!col.isAutoIncrement)
            continue;
        if (!supportsAutoIncrement(databaseType, col.type)) {
            errorMessage = "Column '" + col.name + "' must be an integer type for auto-increment";
            return false;
        }
        if (databaseType == DatabaseType::SQLITE && !col.isPrimaryKey) {
            errorMessage = "SQLite AUTOINCREMENT requires '" + col.name + "' to be a PRIMARY KEY";
            return false;
        }
    }

    return true;
}

bool TableEditorTab::validateColumnInput() {
    errorMessage.clear();

    if (std::strlen(columnName) == 0) {
        errorMessage = "Column name cannot be empty";
        return false;
    }

    if (std::strlen(columnType) == 0) {
        errorMessage = "Data type cannot be empty";
        return false;
    }

    return true;
}

std::string TableEditorTab::generateCreateTableSQL() const {
    if (editingTable.columns.empty()) {
        return "";
    }

    auto builder = createSQLBuilder(databaseType);
    Table table = editingTable;
    table.name = std::string(tableNameBuffer);
    table.comment = std::string(tableCommentBuffer);

    std::string schema;
    if (databaseType == DatabaseType::POSTGRESQL) {
        schema = schemaName.empty() ? "public" : schemaName;
    }

    return builder->createTable(table, schema);
}

std::vector<std::string> TableEditorTab::generateAlterTableStatements() const {
    std::vector<std::string> statements;
    auto builder = createSQLBuilder(databaseType);

    std::string tableName = originalTable.name;
    if (databaseType == DatabaseType::POSTGRESQL && !schemaName.empty()) {
        tableName = schemaName + "." + originalTable.name;
    }

    for (const auto& origCol : originalTable.columns) {
        bool found = false;
        for (const auto& editCol : editingTable.columns) {
            if (origCol.name == editCol.name) {
                found = true;
                break;
            }
        }
        if (!found) {
            statements.push_back(builder->dropColumn(tableName, origCol.name));
        }
    }

    for (const auto& editCol : editingTable.columns) {
        bool found = false;
        for (const auto& origCol : originalTable.columns) {
            if (editCol.name == origCol.name) {
                found = true;
                break;
            }
        }
        if (!found) {
            statements.push_back(builder->addColumn(tableName, editCol));
        }
    }

    return statements;
}

void TableEditorTab::updateCurrentColumn() {
    if (selectedColumnIndex >= 0 &&
        selectedColumnIndex < static_cast<int>(editingTable.columns.size())) {
        auto& column = editingTable.columns[selectedColumnIndex];
        column.name = std::string(columnName);
        column.type = std::string(columnType);
        column.comment = std::string(columnComment);
        column.defaultValue = std::string(defaultValue);
        column.isPrimaryKey = isPrimaryKey;
        column.isNotNull = isNotNull;
        column.isUnique = isUnique;
        column.isAutoIncrement = isAutoIncrement;
        markDirty();
    }
}

void TableEditorTab::markDirty() {
    dirty = true;
}

Table TableEditorTab::buildResultTable() const {
    Table result = editingTable;
    result.name = std::string(tableNameBuffer);
    return result;
}

void TableEditorTab::reset() {
    columnEditMode = ColumnEditMode::None;
    selectedColumnIndex = -1;
    rightPanelMode = RightPanelMode::TableProperties;
    editingTable = Table{};
    originalTable = Table{};
    originalColumnName.clear();
    schemaName.clear();
    std::memset(tableNameBuffer, 0, sizeof(tableNameBuffer));
    std::memset(tableCommentBuffer, 0, sizeof(tableCommentBuffer));
    resetColumnForm();
    errorMessage.clear();
    showPreviewPopup = false;
    previewEditor.SetReadOnly(true);
    previewEditor.SetShowLineNumbers(false);
    dirty = false;
    dbNode = nullptr;
}
