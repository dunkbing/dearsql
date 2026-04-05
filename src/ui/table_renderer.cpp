#include "ui/table_renderer.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <cstring>
#include <format>

TableRenderer::TableRenderer() {
    config.tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
}

TableRenderer::TableRenderer(const Config& config) : config(config) {
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

    const auto& colors = Application::getInstance().getCurrentColors();

    int colCount = static_cast<int>(columns.size());
    if (config.showRowNumbers) {
        colCount++; // Add one for row number column
    }

    float availableHeight = ImGui::GetContentRegionAvail().y;
    if (availableHeight < config.minHeight) {
        availableHeight = config.minHeight;
    }

    if (config.allowEditing && selectedRow >= 0 && selectedCol >= 0 && editingRow == -1 &&
        editingCol == -1 && !config.nonEditableColumns.contains(selectedCol) &&
        (!cellEditableCb || cellEditableCb(selectedRow, selectedCol))) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                enterEditMode(selectedRow, selectedCol);
            } else if (!config.columnDropdownOptions.contains(selectedCol)) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.InputQueueCharacters.Size > 0) {
                    ImWchar c = io.InputQueueCharacters[0];
                    // Check if it's a printable character (not a control character)
                    if (c >= 32 && c != 127) { // 32 is space, 127 is DEL
                        editingRow = selectedRow;
                        editingCol = selectedCol;
                        editBuffer[0] = static_cast<char>(c);
                        editBuffer[1] = '\0';
                        justEnteredEditWithChar = true;
                        initialCursorPos = 1;
                        io.InputQueueCharacters.Size = 0;
                    }
                }
            }
        }
    }

    if (ImGui::BeginTable(tableId, colCount, config.tableFlags, ImVec2(0.0f, availableHeight))) {
        if (config.showRowNumbers) {
            int maxRowNum = rowNumberOffset + static_cast<int>(data.size());
            std::string maxRowStr = std::to_string(maxRowNum);
            float textWidth = ImGui::CalcTextSize(maxRowStr.c_str()).x;
            float columnWidth = textWidth + 10.0f;      // More padding for right alignment
            columnWidth = std::max(columnWidth, 30.0f); // Minimum width
            ImGui::TableSetupColumn("",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoResize |
                                        ImGuiTableColumnFlags_NoHeaderLabel,
                                    columnWidth);
        }

        for (const auto& colName : columns) {
            ImGui::TableSetupColumn(colName.c_str(), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        }

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        // Skip row number column header if present
        if (config.showRowNumbers) {
            ImGui::TableNextColumn();
        }

        for (int colIdx = 0; colIdx < static_cast<int>(columns.size()); colIdx++) {
            ImGui::TableNextColumn();
            renderColumnHeader(colIdx, columns[colIdx]);
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(data.size()));

        while (clipper.Step()) {
            for (int rowIdx = clipper.DisplayStart; rowIdx < clipper.DisplayEnd; rowIdx++) {
                const auto& row = data[rowIdx];
                ImGui::TableNextRow();

                if (config.allowSelection && selectedRow == rowIdx) {
                    ImGui::TableSetBgColor(
                        ImGuiTableBgTarget_RowBg1,
                        ImGui::GetColorU32(
                            ImVec4(colors.surface1.x, colors.surface1.y, colors.surface1.z, 0.4f)));
                }

                if (config.showRowNumbers) {
                    ImGui::TableNextColumn();

                    int rowNum = rowNumberOffset + rowIdx + 1;
                    std::string rowNumStr = std::to_string(rowNum);
                    float textWidth = ImGui::CalcTextSize(rowNumStr.c_str()).x;
                    float columnWidth = ImGui::GetColumnWidth();
                    float padding = 5.0f; // Right padding

                    float cursorX = ImGui::GetCursorPosX();
                    ImGui::SetCursorPosX(cursorX + columnWidth - textWidth - padding);

                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::Text("%d", rowNum);
                    ImGui::PopStyleColor();
                }

                for (int colIdx = 0; colIdx < static_cast<int>(row.size()) &&
                                     colIdx < static_cast<int>(columns.size());
                     colIdx++) {
                    ImGui::TableNextColumn();

                    renderCell(rowIdx, colIdx);

                    if (shouldScrollToCell && rowIdx == scrollTargetRow &&
                        colIdx == scrollTargetCol) {
                        const ImVec2 cellMin = ImGui::GetItemRectMin();
                        const ImVec2 cellMax = ImGui::GetItemRectMax();
                        const ImVec2 windowContentMin = ImGui::GetWindowContentRegionMin();
                        const ImVec2 windowContentMax = ImGui::GetWindowContentRegionMax();
                        const ImVec2 windowPos = ImGui::GetWindowPos();

                        const ImVec2 contentMin = ImVec2(windowPos.x + windowContentMin.x,
                                                         windowPos.y + windowContentMin.y);
                        const ImVec2 contentMax = ImVec2(windowPos.x + windowContentMax.x,
                                                         windowPos.y + windowContentMax.y);

                        const bool cellVisibleX =
                            (cellMax.x > contentMin.x && cellMin.x < contentMax.x);
                        const bool cellVisibleY =
                            (cellMax.y > contentMin.y && cellMin.y < contentMax.y);

                        if (!cellVisibleY) {
                            ImGui::SetScrollHereY(0.5f); // Center the row vertically
                        }

                        if (colIdx == 0) {
                            float currentScrollX = ImGui::GetScrollX();
                            if (currentScrollX > 0.0f) {
                                ImGui::SetScrollHereX(0.0f);
                            }
                        } else if (!cellVisibleX) {
                            float scrollRatio = 0.5f; // Default to center

                            if (colIdx == static_cast<int>(columns.size()) - 1) {
                                // Last column - scroll to right edge
                                scrollRatio = 1.0f;
                            }

                            ImGui::SetScrollHereX(scrollRatio);
                        }

                        shouldScrollToCell = false;
                    }
                }
            }
        }

        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetStyle().ScrollbarSize);
        ImGui::EndTable();
    }
}

void TableRenderer::renderCell(int row, int col) {
    const auto& colors = Application::getInstance().getCurrentColors();

    // Check if this cell is being edited
    if (config.allowEditing && editingRow == row && editingCol == col) {
        // Edit mode - blue tint to indicate editing
        ImGui::TableSetBgColor(
            ImGuiTableBgTarget_CellBg,
            ImGui::GetColorU32(ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.15f)));

        auto dropdownIt = config.columnDropdownOptions.find(col);
        if (dropdownIt != config.columnDropdownOptions.end()) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            if (comboNeedsOpen) {
                ImGui::OpenPopup(ImGui::GetID("##dropdown_edit"));
                comboNeedsOpen = false;
            }
            if (ImGui::BeginCombo("##dropdown_edit", editBuffer, ImGuiComboFlags_NoArrowButton)) {
                comboHasOpened = true;
                for (const auto& option : dropdownIt->second) {
                    bool selected = (option == std::string(editBuffer));
                    if (ImGui::Selectable(option.c_str(), selected)) {
                        strncpy(editBuffer, option.c_str(), sizeof(editBuffer) - 1);
                        editBuffer[sizeof(editBuffer) - 1] = '\0';
                        exitEditMode(true);
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            } else if (comboHasOpened) {
                exitEditMode(false);
            }
            ImGui::PopStyleVar(2);

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                exitEditMode(false);
            }
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SetKeyboardFocusHere();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));

            int extraFlags = 0;
            if (auto it = config.columnInputFlags.find(col); it != config.columnInputFlags.end()) {
                extraFlags = it->second;
            }

            bool shouldExitEditMode = false;
            if (justEnteredEditWithChar) {
                shouldExitEditMode = ImGui::InputText(
                    "##edit", editBuffer, sizeof(editBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways |
                        extraFlags,
                    [](ImGuiInputTextCallbackData* data) {
                        TableRenderer* renderer = static_cast<TableRenderer*>(data->UserData);
                        if (renderer->justEnteredEditWithChar) {
                            data->CursorPos = renderer->initialCursorPos;
                            data->SelectionStart = renderer->initialCursorPos;
                            data->SelectionEnd = renderer->initialCursorPos;
                            renderer->justEnteredEditWithChar = false;
                        }
                        return 0;
                    },
                    this);
            } else {
                shouldExitEditMode =
                    ImGui::InputText("##edit", editBuffer, sizeof(editBuffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue | extraFlags);
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);

            const bool cancelEdit = ImGui::IsKeyPressed(ImGuiKey_Escape);
            const bool deactivateEdit = ImGui::IsItemDeactivated();

            if (cancelEdit) {
                exitEditMode(false);
            } else if (shouldExitEditMode || deactivateEdit) {
                // Commit when the editor loses focus so clicking another cell or toolbar action
                // doesn't leave the edit stuck in the transient buffer.
                exitEditMode(true);
            }
        }
    } else {
        ImGui::PushID(row * static_cast<int>(columns.size()) + col);

        const bool isSelected = (selectedRow == row && selectedCol == col);
        const bool isEdited =
            (row < static_cast<int>(editedCells.size()) &&
             col < static_cast<int>(editedCells[row].size()) && editedCells[row][col]);

        if (isEdited && isSelected) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.teal.x, colors.teal.y, colors.teal.z, 0.5f)));
        } else if (isEdited) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.teal.x, colors.teal.y, colors.teal.z, 0.3f)));
        } else if (isSelected) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface2));
        }

        const std::string& cellValue = data[row][col];

        bool hasColorOverride = false;
        if (cellColorCb) {
            if (ImU32 color = cellColorCb(row, col, cellValue); color != 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                hasColorOverride = true;
            }
        }

        if (config.allowSelection) {
            handleCellInteraction(row, col, isSelected);
        } else {
            ImGui::Text("%s", cellValue.c_str());

            if (ImGui::IsItemHovered() && cellValue.length() > 50) {
                ImGui::SetTooltip("%s", cellValue.c_str());
            }
        }

        if (hasColorOverride)
            ImGui::PopStyleColor();

        ImGui::PopID();
    }
}

void TableRenderer::handleCellInteraction(int row, int col, bool isSelected) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const std::string& cellValue = data[row][col];

    // Make Selectable transparent - cell bg is handled by TableSetBgColor
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

    if (ImGui::Selectable(cellValue.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
        selectedRow = row;
        selectedCol = col;

        if (onCellSelect) {
            onCellSelect(row, col);
        }

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (config.allowEditing) {
                enterEditMode(row, col);
            }

            if (onCellDoubleClick) {
                onCellDoubleClick(row, col);
            }
        }
    }

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() && !isSelected && !config.nonEditableColumns.contains(col) &&
        (!cellEditableCb || cellEditableCb(row, col))) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface1));
    }

    if (ImGui::IsItemHovered() && cellValue.length() > 50) {
        ImGui::SetTooltip("%s", cellValue.c_str());
    }
}

void TableRenderer::enterEditMode(int row, int col) {
    if (!config.allowEditing || config.nonEditableColumns.contains(col))
        return;
    if (cellEditableCb && !cellEditableCb(row, col))
        return;

    if (row >= 0 && row < static_cast<int>(data.size()) && col >= 0 &&
        col < static_cast<int>(columns.size())) {
        editingRow = row;
        editingCol = col;

        const std::string& currentValue = data[row][col];
        strncpy(editBuffer, currentValue.c_str(), sizeof(editBuffer) - 1);
        editBuffer[sizeof(editBuffer) - 1] = '\0';

        if (config.columnDropdownOptions.contains(col)) {
            comboNeedsOpen = true;
            comboHasOpened = false;
        }
    }
}

void TableRenderer::exitEditMode(bool saveEdit) {
    if (editingRow >= 0 && editingCol >= 0) {
        if (saveEdit && onCellEdit) {
            std::string newValue = editBuffer;
            onCellEdit(editingRow, editingCol, newValue);
        }

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

    shouldScrollToCell = true;
    scrollTargetRow = row;
    scrollTargetCol = col;
}

void TableRenderer::renderColumnHeader(int colIdx, const std::string& colName) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const bool isSorted = (sortColumn == colIdx && sortDirection != SortDirection::None);
    const std::string popupId = std::format("##sort_popup_{}", colIdx);

    float columnWidth = ImGui::GetColumnWidth();

    ImGui::Text("%s", colName.c_str());

    if (isSorted) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.blue);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));

        const char* icon = sortDirection == SortDirection::Ascending
                               ? ICON_FA_ARROW_UP_SHORT_WIDE
                               : ICON_FA_ARROW_DOWN_WIDE_SHORT;
        if (ImGui::SmallButton(icon)) {
            sortColumn = -1;
            sortDirection = SortDirection::None;
            if (onSortChanged) {
                onSortChanged(-1, "", SortDirection::None);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to clear sort");
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    ImGui::SameLine();

    float chevronWidth =
        ImGui::CalcTextSize(ICON_FA_CHEVRON_DOWN).x + ImGui::GetStyle().FramePadding.x * 2;
    float availableWidth = ImGui::GetContentRegionAvail().x;
    if (availableWidth > chevronWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableWidth - chevronWidth);
    }

    ImGui::PushID(colIdx);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);

    if (ImGui::SmallButton(ICON_FA_CHEVRON_DOWN)) {
        ImGui::OpenPopup(popupId.c_str());
    }

    ImGui::PopStyleColor(3);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    if (ImGui::BeginPopup(popupId.c_str())) {
        bool isAsc = (sortColumn == colIdx && sortDirection == SortDirection::Ascending);
        if (ImGui::MenuItem(ICON_FA_ARROW_UP_SHORT_WIDE " Order by ASC", nullptr, isAsc)) {
            if (isAsc) {
                sortColumn = -1;
                sortDirection = SortDirection::None;
                if (onSortChanged) {
                    onSortChanged(-1, "", SortDirection::None);
                }
            } else {
                sortColumn = colIdx;
                sortDirection = SortDirection::Ascending;
                if (onSortChanged) {
                    onSortChanged(colIdx, colName, SortDirection::Ascending);
                }
            }
        }

        bool isDesc = (sortColumn == colIdx && sortDirection == SortDirection::Descending);
        if (ImGui::MenuItem(ICON_FA_ARROW_DOWN_WIDE_SHORT " Order by DESC", nullptr, isDesc)) {
            if (isDesc) {
                sortColumn = -1;
                sortDirection = SortDirection::None;
                if (onSortChanged) {
                    onSortChanged(-1, "", SortDirection::None);
                }
            } else {
                sortColumn = colIdx;
                sortDirection = SortDirection::Descending;
                if (onSortChanged) {
                    onSortChanged(colIdx, colName, SortDirection::Descending);
                }
            }
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    ImGui::PopID();
}
