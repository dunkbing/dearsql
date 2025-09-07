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
            // Calculate width needed for row numbers
            int maxRowNum = rowNumberOffset + static_cast<int>(data.size());
            std::string maxRowStr = std::to_string(maxRowNum);
            float textWidth = ImGui::CalcTextSize(maxRowStr.c_str()).x;
            // Just add minimal padding for the column
            float columnWidth = textWidth + 5.0f;       // Simplified padding
            columnWidth = std::max(columnWidth, 15.0f); // Minimum width for header "#"
            ImGui::TableSetupColumn(
                "#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
                columnWidth);
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
                ImGui::Text("%d", rowNumberOffset + rowIdx + 1);
            }

            // Data columns
            for (int colIdx = 0;
                 colIdx < static_cast<int>(row.size()) && colIdx < static_cast<int>(columns.size());
                 colIdx++) {
                ImGui::TableNextColumn();

                renderCell(rowIdx, colIdx);

                // Handle scrolling to target cell after rendering (so we can check visibility)
                if (shouldScrollToCell && rowIdx == scrollTargetRow && colIdx == scrollTargetCol) {
                    // Check if the cell is actually visible before scrolling
                    const ImVec2 cellMin = ImGui::GetItemRectMin();
                    const ImVec2 cellMax = ImGui::GetItemRectMax();
                    const ImVec2 windowContentMin = ImGui::GetWindowContentRegionMin();
                    const ImVec2 windowContentMax = ImGui::GetWindowContentRegionMax();
                    const ImVec2 windowPos = ImGui::GetWindowPos();

                    // Convert to absolute coordinates
                    const ImVec2 contentMin =
                        ImVec2(windowPos.x + windowContentMin.x, windowPos.y + windowContentMin.y);
                    const ImVec2 contentMax =
                        ImVec2(windowPos.x + windowContentMax.x, windowPos.y + windowContentMax.y);

                    // Check if cell is visible horizontally
                    const bool cellVisibleX =
                        (cellMax.x > contentMin.x && cellMin.x < contentMax.x);
                    // Check if cell is visible vertically
                    const bool cellVisibleY =
                        (cellMax.y > contentMin.y && cellMin.y < contentMax.y);

                    // Only scroll if the cell is not fully visible
                    if (!cellVisibleY) {
                        ImGui::SetScrollHereY(0.5f); // Center the row vertically
                    }

                    // For horizontal scrolling, handle first column specially
                    if (colIdx == 0) {
                        // Always scroll to the leftmost position for the first column
                        // Check if we're not already at the leftmost scroll position
                        float currentScrollX = ImGui::GetScrollX();
                        if (currentScrollX > 0.0f) {
                            ImGui::SetScrollHereX(0.0f);
                        }
                    } else if (!cellVisibleX) {
                        // For other columns, only scroll if not visible
                        float scrollRatio = 0.5f; // Default to center

                        if (colIdx == static_cast<int>(columns.size()) - 1) {
                            // Last column - scroll to right edge
                            scrollRatio = 1.0f;
                        }

                        ImGui::SetScrollHereX(scrollRatio);
                    }

                    // Reset the scroll flag after checking
                    shouldScrollToCell = false;
                }
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

void TableRenderer::scrollToCell(int row, int col) {
    if (row < 0 || row >= static_cast<int>(data.size()) || col < 0 ||
        col >= static_cast<int>(columns.size())) {
        return;
    }

    // Set flag to scroll to this cell on next render
    shouldScrollToCell = true;
    scrollTargetRow = row;
    scrollTargetCol = col;
}
