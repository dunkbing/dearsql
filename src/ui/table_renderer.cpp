#include "ui/table_renderer.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/db.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <algorithm>
#include <cstring>
#include <format>

namespace {
    // truncate at first newline for single-line cell display
    std::string getFirstLine(const std::string& text) {
        auto pos = text.find('\n');
        if (pos == std::string::npos)
            return text;
        return text.substr(0, pos);
    }
} // namespace

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

void TableRenderer::setColumns(const std::vector<Column>& cols) {
    columns = cols;
}

void TableRenderer::setColumns(const std::vector<std::string>& columnNames) {
    columns.clear();
    columns.reserve(columnNames.size());
    for (const auto& name : columnNames) {
        Column col;
        col.name = name;
        columns.push_back(std::move(col));
    }
}

void TableRenderer::setData(const std::vector<std::vector<std::string>>& tableData) {
    data = tableData;
}

void TableRenderer::setData(std::vector<std::vector<std::string>>&& tableData) {
    data = std::move(tableData);
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
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::GetIO().WantTextInput) {
            // boolean cells: toggle on Enter/Space, ignore typed chars
            bool isBoolCell = selectedRow < static_cast<int>(data.size()) &&
                              selectedCol < static_cast<int>(data[selectedRow].size()) &&
                              isBoolSentinel(data[selectedRow][selectedCol]);

            if (isBoolCell) {
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space)) {
                    bool cur = boolSentinelValue(data[selectedRow][selectedCol]);
                    if (onCellEdit) {
                        onCellEdit(selectedRow, selectedCol,
                                   std::string(cur ? BOOL_FALSE_SENTINEL : BOOL_TRUE_SENTINEL));
                    }
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                enterEditMode(selectedRow, selectedCol);
            } else if (!config.columnDropdownOptions.contains(selectedCol) &&
                       (selectedCol >= static_cast<int>(columns.size()) ||
                        !DateTimePicker::columnKind(columns[selectedCol].type))) {
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

        for (const auto& col : columns) {
            ImGui::TableSetupColumn(col.name.c_str(), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        }

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        // Skip row number column header if present
        if (config.showRowNumbers) {
            ImGui::TableNextColumn();
        }

        for (int colIdx = 0; colIdx < static_cast<int>(columns.size()); colIdx++) {
            ImGui::TableNextColumn();
            renderColumnHeader(colIdx, columns[colIdx].name);
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

    if (pendingEditOverlay) {
        renderEditOverlay();
        pendingEditOverlay = false;
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
        } else if (col < static_cast<int>(columns.size()) &&
                   DateTimePicker::columnKind(columns[col].type)) {
            // date/datetime column: show picker popup
            const bool showNull = editBuffer[0] == '\0' || isNullSentinel(editBuffer);
            const std::string displayValue = showNull ? "NULL" : getFirstLine(editBuffer);
            ImGui::Text("%s", displayValue.c_str());
            const ImVec2 popupPos = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);

            if (datePickerNeedsOpen) {
                ImGui::OpenPopup("##dtp");
                datePickerNeedsOpen = false;
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
            ImGui::PushStyleColor(ImGuiCol_PopupBg,
                                  Application::getInstance().getCurrentColors().base);
            ImGui::PushStyleColor(ImGuiCol_Border,
                                  Application::getInstance().getCurrentColors().overlay0);

            // ensure popup is tall enough for 6 calendar rows + header + day labels
            float rowH = ImGui::GetFrameHeight();
            float minH =
                ImGui::GetStyle().WindowPadding.y * 2.0f + ImGui::GetFrameHeightWithSpacing() +
                ImGui::GetTextLineHeightWithSpacing() + rowH * 6.0f + Theme::Spacing::XS * 5.0f;
            ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, minH), ImVec2(FLT_MAX, FLT_MAX));

            if (ImGui::BeginPopup("##dtp")) {
                auto result = DateTimePicker::render(dtPickerState);
                if (result.setNull) {
                    const std::string nullSentinel = std::string(NULL_SENTINEL);
                    strncpy(editBuffer, nullSentinel.c_str(), sizeof(editBuffer) - 1);
                    editBuffer[sizeof(editBuffer) - 1] = '\0';
                    datePickerDirty = true;
                    exitEditMode(true);
                    ImGui::CloseCurrentPopup();
                } else if (result.committed) {
                    strncpy(editBuffer, result.value.c_str(), sizeof(editBuffer) - 1);
                    editBuffer[sizeof(editBuffer) - 1] = '\0';
                    datePickerDirty = true;
                    exitEditMode(true);
                    ImGui::CloseCurrentPopup();
                } else if (result.changed) {
                    strncpy(editBuffer, result.value.c_str(), sizeof(editBuffer) - 1);
                    editBuffer[sizeof(editBuffer) - 1] = '\0';
                    datePickerDirty = true;
                }
                ImGui::EndPopup();
            } else if (editingRow >= 0 && editingCol >= 0) {
                exitEditMode(datePickerDirty);
            }

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                exitEditMode(false);
            }
        } else {
            // store position for overlay edit input rendered after EndTable
            editOverlayPosX = ImGui::GetCursorScreenPos().x;
            editOverlayPosY = ImGui::GetCursorScreenPos().y;
            editOverlayWidth = ImGui::GetColumnWidth();
            pendingEditOverlay = true;

            // single-line placeholder in cell
            std::string firstLine = getFirstLine(data[row][col]);
            ImGui::Text("%s", firstLine.empty() ? " " : firstLine.c_str());
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
            if (isBoolSentinel(cellValue)) {
                bool checked = boolSentinelValue(cellValue);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
                float checkboxWidth = ImGui::GetFrameHeight();
                float columnWidth = ImGui::GetColumnWidth();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - checkboxWidth) * 0.5f);
                ImGui::BeginDisabled();
                ImGui::Checkbox("##bool", &checked);
                ImGui::EndDisabled();
                ImGui::PopStyleVar();
            } else if (isNullSentinel(cellValue)) {
                ImGui::TextColored(colors.overlay1, "NULL");
            } else {
                ImGui::Text("%s", getFirstLine(cellValue).c_str());
            }

            if (ImGui::IsItemHovered() &&
                (cellValue.length() > 50 || cellValue.find('\n') != std::string::npos)) {
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
    const bool isNull = isNullSentinel(cellValue);
    const bool isBool = isBoolSentinel(cellValue);

    // boolean cells: render a small centered checkbox
    if (isBool) {
        bool checked = boolSentinelValue(cellValue);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
        float checkboxWidth = ImGui::GetFrameHeight();
        float columnWidth = ImGui::GetColumnWidth();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - checkboxWidth) * 0.5f);

        // make the checkbox background transparent so cell bg shows through
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));

        bool editable = config.allowEditing && !config.nonEditableColumns.contains(col) &&
                        (!cellEditableCb || cellEditableCb(row, col));
        if (!editable)
            ImGui::BeginDisabled();

        if (ImGui::Checkbox("##bool", &checked)) {
            selectedRow = row;
            selectedCol = col;
            if (onCellSelect)
                onCellSelect(row, col);
            if (onCellEdit) {
                onCellEdit(row, col,
                           std::string(checked ? BOOL_TRUE_SENTINEL : BOOL_FALSE_SENTINEL));
            }
        }

        if (!editable)
            ImGui::EndDisabled();

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        // select on click even if not toggling
        if (ImGui::IsItemClicked() || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            selectedRow = row;
            selectedCol = col;
            if (onCellSelect)
                onCellSelect(row, col);
        }

        if (ImGui::IsItemHovered() && !isSelected && editable) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface1));
        }

        // right-click: allow Set NULL on boolean columns
        if (config.allowEditing && onSetNull && columnNullableCb && columnNullableCb(col)) {
            const std::string menuId = std::format("##cell_ctx_{}_{}", row, col);
            if (ImGui::BeginPopupContextItem(menuId.c_str())) {
                if (selectedRow != row || selectedCol != col) {
                    selectedRow = row;
                    selectedCol = col;
                    if (onCellSelect)
                        onCellSelect(row, col);
                }
                if (ImGui::MenuItem("Set NULL")) {
                    onSetNull(row, col);
                }
                ImGui::EndPopup();
            }
        }
        return;
    }

    std::string singleLineText;
    const char* displayText;
    if (isNull) {
        displayText = "NULL";
    } else {
        auto nlPos = cellValue.find('\n');
        if (nlPos != std::string::npos) {
            singleLineText = cellValue.substr(0, nlPos);
            displayText = singleLineText.c_str();
        } else {
            displayText = cellValue.c_str();
        }
    }

    // Make Selectable transparent - cell bg is handled by TableSetBgColor
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

    if (isNull)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.overlay1));

    if (ImGui::Selectable(displayText, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
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

    if (isNull)
        ImGui::PopStyleColor();

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() && !isSelected && !config.nonEditableColumns.contains(col) &&
        (!cellEditableCb || cellEditableCb(row, col))) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface1));
    }

    if (ImGui::IsItemHovered() && !isNull &&
        (cellValue.length() > 50 || cellValue.find('\n') != std::string::npos)) {
        ImGui::SetTooltip("%s", cellValue.c_str());
    }

    // right-click context menu — only when there's something to show
    if (config.allowEditing && onSetNull && !isNull && columnNullableCb && columnNullableCb(col)) {
        const std::string menuId = std::format("##cell_ctx_{}_{}", row, col);
        if (ImGui::BeginPopupContextItem(menuId.c_str())) {
            if (selectedRow != row || selectedCol != col) {
                selectedRow = row;
                selectedCol = col;
                if (onCellSelect)
                    onCellSelect(row, col);
            }
            if (ImGui::MenuItem("Set NULL")) {
                onSetNull(row, col);
            }
            ImGui::EndPopup();
        }
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
        if (isNullSentinel(currentValue)) {
            editBuffer[0] = '\0';
        } else {
            strncpy(editBuffer, currentValue.c_str(), sizeof(editBuffer) - 1);
            editBuffer[sizeof(editBuffer) - 1] = '\0';
        }

        if (config.columnDropdownOptions.contains(col)) {
            comboNeedsOpen = true;
            comboHasOpened = false;
        } else if (int kind = DateTimePicker::columnKind(columns[col].type); kind > 0) {
            dtPickerState.kind = kind;
            dtPickerState.allowNull = !columnNullableCb || columnNullableCb(col);
            dtPickerState.suffix.clear();
            if (!DateTimePicker::parse(currentValue, dtPickerState)) {
                // default to now
                auto now = std::chrono::system_clock::now();
                std::time_t t = std::chrono::system_clock::to_time_t(now);
#ifdef _MSC_VER
                localtime_s(&dtPickerState.date, &t);
#else
                localtime_r(&t, &dtPickerState.date);
#endif
                dtPickerState.suffix.clear();
                dtPickerState.hour = dtPickerState.minute = dtPickerState.second = 0;
            }
            dtPickerState.scrollTime = true;
            datePickerNeedsOpen = true;
            datePickerDirty = false;
        }
        editOverlayFrames = 0;
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
        datePickerNeedsOpen = false;
        datePickerDirty = false;
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

// render a button with an optional keyboard shortcut badge
static bool buttonWithBadge(const char* label, const char* badge, const Theme::Colors& colors) {
    auto& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float labelW = ImGui::CalcTextSize(label).x;
    float h = ImGui::GetFrameHeight();
    float badgeTextW = badge ? ImGui::CalcTextSize(badge).x : 0;
    float badgePadX = Theme::Spacing::S;
    float badgeFullW = badge ? badgeTextW + badgePadX * 2 : 0;
    float gap = badge ? Theme::Spacing::S : 0;
    float totalW = style.FramePadding.x + labelW + gap + badgeFullW + style.FramePadding.x;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool pressed = ImGui::InvisibleButton("##btn", ImVec2(totalW, h));
    ImGui::PopID();
    bool hovered = ImGui::IsItemHovered();

    ImU32 bgCol = hovered ? ImGui::GetColorU32(colors.surface1) : IM_COL32(0, 0, 0, 0);
    ImVec2 max = ImVec2(pos.x + totalW, pos.y + h);
    dl->AddRectFilled(pos, max, bgCol, style.FrameRounding);
    dl->AddRect(pos, max, ImGui::GetColorU32(colors.overlay0), style.FrameRounding);

    float textY = pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(pos.x + style.FramePadding.x, textY), ImGui::GetColorU32(ImGuiCol_Text),
                label);

    if (badge) {
        float badgeH = h - Theme::Spacing::S;
        float badgeX = pos.x + style.FramePadding.x + labelW + gap;
        float badgeY = pos.y + (h - badgeH) * 0.5f;
        ImVec2 bMin(badgeX, badgeY);
        ImVec2 bMax(badgeX + badgeFullW, badgeY + badgeH);
        dl->AddRectFilled(bMin, bMax, ImGui::GetColorU32(colors.surface1), 3.0f);
        dl->AddRect(bMin, bMax, ImGui::GetColorU32(colors.overlay0), 3.0f);
        dl->AddText(
            ImVec2(badgeX + badgePadX, badgeY + (badgeH - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(colors.overlay1), badge);
    }

    return pressed;
}

void TableRenderer::renderEditOverlay() {
    if (editingRow < 0 || editingCol < 0)
        return;

    const auto& colors = Application::getInstance().getCurrentColors();

    float width = std::max(editOverlayWidth, 250.0f);
    int lineCount = 1;
    for (const char* p = editBuffer; *p; ++p) {
        if (*p == '\n')
            lineCount++;
    }
    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    float lines = std::min(std::max(static_cast<float>(lineCount + 1), 3.0f), 15.0f);
    float buttonBarH = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::XS;
    float height = lineHeight * lines + 12.0f + buttonBarH;

    ImGui::SetNextWindowPos(ImVec2(editOverlayPosX, editOverlayPosY));
    ImGui::SetNextWindowSize(ImVec2(width, height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.blue);

    bool wantSave = false;
    bool wantCancel = false;
    bool wantSetNull = false;

    if (ImGui::Begin("##edit_overlay", nullptr, flags)) {
        if (editOverlayFrames < 3) {
            ImGui::SetKeyboardFocusHere();
            editOverlayFrames++;
        }

        int extraFlags = 0;
        if (auto it = config.columnInputFlags.find(editingCol);
            it != config.columnInputFlags.end()) {
            extraFlags = it->second;
        }

        float inputH = ImGui::GetContentRegionAvail().y - buttonBarH;

        if (justEnteredEditWithChar) {
            ImGui::InputTextMultiline(
                "##edit_overlay_input", editBuffer, sizeof(editBuffer), ImVec2(-FLT_MIN, inputH),
                ImGuiInputTextFlags_CallbackAlways | extraFlags,
                [](ImGuiInputTextCallbackData* cbData) {
                    auto* renderer = static_cast<TableRenderer*>(cbData->UserData);
                    if (renderer->justEnteredEditWithChar) {
                        cbData->CursorPos = renderer->initialCursorPos;
                        cbData->SelectionStart = renderer->initialCursorPos;
                        cbData->SelectionEnd = renderer->initialCursorPos;
                        renderer->justEnteredEditWithChar = false;
                    }
                    return 0;
                },
                this);
        } else {
            ImGui::InputTextMultiline("##edit_overlay_input", editBuffer, sizeof(editBuffer),
                                      ImVec2(-FLT_MIN, inputH), extraFlags);
        }

        // button bar
        ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));

        bool showSetNull = columnNullableCb && columnNullableCb(editingCol);
        if (showSetNull) {
            if (buttonWithBadge("Set NULL", nullptr, colors))
                wantSetNull = true;
            ImGui::SameLine();
        }

#ifdef __APPLE__
        const char* saveShortcut = "Cmd+Enter";
#else
        const char* saveShortcut = "Ctrl+Enter";
#endif

        // right-align Cancel and Save
        auto& style = ImGui::GetStyle();
        float cancelW = style.FramePadding.x + ImGui::CalcTextSize("Cancel").x + Theme::Spacing::S +
                        ImGui::CalcTextSize("Esc").x + Theme::Spacing::S * 2 + style.FramePadding.x;
        float saveW = style.FramePadding.x + ImGui::CalcTextSize("Save").x + Theme::Spacing::S +
                      ImGui::CalcTextSize(saveShortcut).x + Theme::Spacing::S * 2 +
                      style.FramePadding.x;
        float rightW = cancelW + style.ItemSpacing.x + saveW;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > rightW)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - rightW);

        if (buttonWithBadge("Cancel", "Esc", colors))
            wantCancel = true;
        ImGui::SameLine();
        if (buttonWithBadge("Save", saveShortcut, colors))
            wantSave = true;

        // Ctrl/Cmd+Enter saves
        if ((ImGui::GetIO().KeyMods & ImGuiMod_Shortcut) &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
            wantSave = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            wantCancel = true;
        } else if (editOverlayFrames >= 3 &&
                   !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            wantSave = true;
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (wantSetNull) {
        if (onSetNull)
            onSetNull(editingRow, editingCol);
        exitEditMode(false);
    } else if (wantCancel) {
        exitEditMode(false);
    } else if (wantSave) {
        exitEditMode(true);
    }
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
