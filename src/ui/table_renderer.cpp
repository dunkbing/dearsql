#include "ui/table_renderer.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <cstring>

TableRenderer::TableRenderer() {
    // Set default table flags
    config.tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
}

TableRenderer::TableRenderer(const Config& config) : config(config) {
    // Set default table flags if none provided
    if (this->config.tableFlags == 0) {
        this->config.tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable;
    }
}

void TableRenderer::setColumns(const std::vector<std::string>& columnNames) {
    columns = columnNames;
}

void TableRenderer::setData(const std::vector<std::vector<std::string>>& tableData) {
    data = tableData;
}

void TableRenderer::setCellEditedStatus(const std::vector<std::vector<bool>>& editedCellsStatus) {
    editedCells = editedCellsStatus;
}

void TableRenderer::setSelectedCell(int row, int col) {
    selectedRow = row;
    selectedCol = col;
}

void TableRenderer::render(const char* tableId) {
    if (columns.empty()) {
        ImGui::Text("No columns defined");
        return;
    }

    if (data.empty()) {
        ImGui::Text("No data to display");
        return;
    }

    const auto& colors =
        Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    int colCount = static_cast<int>(columns.size());
    if (config.showRowNumbers) {
        colCount++; // Add one for row number column
    }

    float availableHeight = ImGui::GetContentRegionAvail().y;
    if (availableHeight < config.minHeight) {
        availableHeight = config.minHeight;
    }

    if (ImGui::BeginTable(tableId, colCount, config.tableFlags, ImVec2(0.0f, availableHeight))) {
        // Setup columns
        if (config.showRowNumbers) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        }

        for (const auto& colName : columns) {
            ImGui::TableSetupColumn(colName.c_str(), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        }
        ImGui::TableHeadersRow();

        // Render data rows
        for (int rowIdx = 0; rowIdx < static_cast<int>(data.size()); rowIdx++) {
            const auto& row = data[rowIdx];
            ImGui::TableNextRow();

            // Row number column
            if (config.showRowNumbers) {
                ImGui::TableNextColumn();
                ImGui::Text("%d", rowIdx + 1);
            }

            // Data columns
            for (int colIdx = 0;
                 colIdx < static_cast<int>(row.size()) && colIdx < static_cast<int>(columns.size());
                 colIdx++) {
                ImGui::TableNextColumn();
                renderCell(rowIdx, colIdx);
            }
        }

        ImGui::EndTable();
    }
}

void TableRenderer::renderCell(int row, int col) {
    const auto& colors =
        Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    // Check if this cell is being edited
    if (config.allowEditing && editingRow == row && editingCol == col) {
        // Edit mode - show input field
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##edit", editBuffer, sizeof(editBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            exitEditMode(true);
        }
        // Exit edit mode on Escape
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            exitEditMode(false);
        }
    } else {
        // Display mode - show cell content
        ImGui::PushID(row * static_cast<int>(columns.size()) + col);

        const bool isSelected = (selectedRow == row && selectedCol == col);
        const bool isEdited =
            (row < static_cast<int>(editedCells.size()) &&
             col < static_cast<int>(editedCells[row].size()) && editedCells[row][col]);

        // Apply cell background colors
        if (isEdited) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.teal.x, colors.teal.y, colors.teal.z, 0.3f)));
        } else if (isSelected) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface2));
        }

        const std::string& cellValue = data[row][col];

        if (config.allowSelection) {
            handleCellInteraction(row, col, isSelected);
        } else {
            // Just display the text
            ImGui::Text("%s", cellValue.c_str());

            // Add tooltip for long text
            if (ImGui::IsItemHovered() && cellValue.length() > 50) {
                ImGui::SetTooltip("%s", cellValue.c_str());
            }
        }

        ImGui::PopID();
    }
}

void TableRenderer::handleCellInteraction(int row, int col, bool isSelected) {
    const std::string& cellValue = data[row][col];

    if (ImGui::Selectable(cellValue.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
        // Single click - select cell
        selectedRow = row;
        selectedCol = col;

        if (onCellSelect) {
            onCellSelect(row, col);
        }

        // Double click - enter edit mode or trigger callback
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (config.allowEditing) {
                enterEditMode(row, col);
            }

            if (onCellDoubleClick) {
                onCellDoubleClick(row, col);
            }
        }
    }

    // Add tooltip for long text
    if (ImGui::IsItemHovered() && cellValue.length() > 50) {
        ImGui::SetTooltip("%s", cellValue.c_str());
    }
}

void TableRenderer::enterEditMode(int row, int col) {
    if (!config.allowEditing)
        return;

    if (row >= 0 && row < static_cast<int>(data.size()) && col >= 0 &&
        col < static_cast<int>(columns.size())) {
        editingRow = row;
        editingCol = col;

        // Copy current cell value to edit buffer
        const std::string& currentValue = data[row][col];
        strncpy(editBuffer, currentValue.c_str(), sizeof(editBuffer) - 1);
        editBuffer[sizeof(editBuffer) - 1] = '\0';
    }
}

void TableRenderer::exitEditMode(bool saveEdit) {
    if (editingRow >= 0 && editingCol >= 0) {
        if (saveEdit && onCellEdit) {
            std::string newValue = editBuffer;
            onCellEdit(editingRow, editingCol, newValue);
        }

        // Clear edit state
        editingRow = -1;
        editingCol = -1;
        memset(editBuffer, 0, sizeof(editBuffer));
    }
}
