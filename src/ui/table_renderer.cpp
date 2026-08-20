#include "ui/table_renderer.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/db.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "themes.hpp"
#include "ui/table_aurora_shader.hpp"
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

    // RFC 4180: wrap in quotes and double any embedded quote when the field
    // contains a separator, quote, or newline.
    std::string csvEscape(const std::string& field) {
        const bool needsQuoting = field.find_first_of(",\"\n\r") != std::string::npos;
        if (!needsQuoting)
            return field;
        std::string out;
        out.reserve(field.size() + 2);
        out.push_back('"');
        for (char c : field) {
            if (c == '"')
                out.push_back('"');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    // render cell value for clipboard: unwrap null/bool sentinels.
    std::string exportValue(const std::string& v) {
        if (isNullSentinel(v))
            return {};
        if (isBoolSentinel(v))
            return boolSentinelValue(v) ? "true" : "false";
        return v;
    }

    bool paddedMenuItem(const char* label, bool enabled = true) {
        const auto& colors = Application::getInstance().getCurrentColors();
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const float padX = style.FramePadding.x + Theme::Spacing::S;
        const float padY = style.FramePadding.y + Theme::Spacing::XS;
        const float width = std::max(ImGui::GetContentRegionAvail().x, textSize.x + padX * 2.0f);
        const float height = textSize.y + padY * 2.0f;

        if (!enabled)
            ImGui::BeginDisabled();
        const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (!enabled)
            ImGui::EndDisabled();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (enabled && (hovered || active)) {
            drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                                    ImGui::GetColorU32(active ? colors.surface2 : colors.surface1),
                                    style.FrameRounding);
        }

        const ImU32 textColor = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        drawList->AddText(ImVec2(pos.x + padX, pos.y + padY), textColor, label);

        if (pressed)
            ImGui::CloseCurrentPopup();
        return pressed && enabled;
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

void TableRenderer::setRowsPendingDelete(const std::vector<bool>& rows) {
    rowsPendingDelete = rows;
}

void TableRenderer::setSelectedCell(int row, int col) {
    const bool sameCell = (selectedRow == row && selectedCol == col);
    selectedRow = row;
    selectedCol = col;
    if (row < 0 || col < 0) {
        rangeAnchorRow = -1;
        rangeAnchorCol = -1;
        rangeEndRow = -1;
        rangeEndCol = -1;
        isDragging = false;
        return;
    }

    if (rangeAnchorRow < 0 || rangeAnchorCol < 0 || rangeEndRow < 0 || rangeEndCol < 0 ||
        !sameCell) {
        collapseSelectionToCell(row, col);
    }
}

std::vector<int> TableRenderer::getSelectedRows() const {
    if (rangeAnchorRow < 0 || rangeEndRow < 0) {
        return {};
    }

    const int firstRow = std::max(0, std::min(rangeAnchorRow, rangeEndRow));
    const int lastRow =
        std::min(static_cast<int>(data.size()) - 1, std::max(rangeAnchorRow, rangeEndRow));
    if (firstRow > lastRow) {
        return {};
    }

    std::vector<int> rows;
    rows.reserve(static_cast<size_t>(lastRow - firstRow + 1));
    for (int row = firstRow; row <= lastRow; ++row) {
        rows.push_back(row);
    }
    return rows;
}

void TableRenderer::collapseSelectionToCell(int row, int col) {
    selectedRow = row;
    selectedCol = col;
    rangeAnchorRow = row;
    rangeAnchorCol = col;
    rangeEndRow = row;
    rangeEndCol = col;
}

void TableRenderer::setSelectionRange(int anchorRow, int anchorCol, int endRow, int endCol) {
    rangeAnchorRow = anchorRow;
    rangeAnchorCol = anchorCol;
    rangeEndRow = endRow;
    rangeEndCol = endCol;
    selectedRow = endRow;
    selectedCol = endCol;
}

void TableRenderer::updateDragFromItem(int row, int col) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;
    if (ImGui::GetIO().KeyShift)
        return;

    // use the cell's full bg rect (including padding) for hit-testing instead
    // of GetItemRectMin/Max, which would only cover the inner widget — a
    // narrow checkbox or short Selectable text wouldn't register most of the cell.
    ImGuiTable* table = ImGui::GetCurrentTable();
    if (!table)
        return;
    const int tableColIdx = config.showRowNumbers ? col + 1 : col;
    const ImRect cellRect = ImGui::TableGetCellBgRect(table, tableColIdx);

    if (!cellRect.Contains(ImGui::GetMousePos()))
        return;

    if (!isDragging) {
        // only start a drag if the press happened this frame —
        // prevents a mouse that was pressed outside the table from
        // initiating a selection the moment it enters a cell
        if (!leftPressedThisFrame)
            return;
        collapseSelectionToCell(row, col);
        isDragging = true;
        if (onCellSelect)
            onCellSelect(row, col);
        return;
    }

    if (rangeEndRow != row || rangeEndCol != col) {
        rangeEndRow = row;
        rangeEndCol = col;
        selectedRow = row;
        selectedCol = col;
        if (onCellSelect)
            onCellSelect(row, col);
    }
}

bool TableRenderer::isInSelectionRange(int row, int col) const {
    if (rangeAnchorRow < 0 || rangeEndRow < 0)
        return false;
    const int r0 = std::min(rangeAnchorRow, rangeEndRow);
    const int r1 = std::max(rangeAnchorRow, rangeEndRow);
    const int c0 = std::min(rangeAnchorCol, rangeEndCol);
    const int c1 = std::max(rangeAnchorCol, rangeEndCol);
    return row >= r0 && row <= r1 && col >= c0 && col <= c1;
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

    // reconcile drag/mouse state with current mouse state at frame start
    const bool mouseDownNow = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool releasedThisFrame = !mouseDownNow && mouseWasDownLastFrame;
    leftPressedThisFrame = mouseDownNow && !mouseWasDownLastFrame;
    if (isDragging && !mouseDownNow) {
        isDragging = false;
    }
    mouseWasDownLastFrame = mouseDownNow;

    // Cmd/Ctrl+A → select every cell when the table already has a selection.
    if (config.allowSelection && editingRow == -1 && selectedRow >= 0 && selectedCol >= 0 &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_A)) {
        const int lastRow = static_cast<int>(data.size()) - 1;
        const int lastCol = static_cast<int>(columns.size()) - 1;
        setSelectionRange(0, 0, lastRow, lastCol);
        if (onCellSelect) {
            onCellSelect(lastRow, lastCol);
        }
    }

    // Cmd/Ctrl+C → copy the selected range as CSV.
    // Gated on the window being focused and no text input active so it doesn't
    // compete with InputText's own copy or the edit overlay.
    if (config.allowSelection && editingRow == -1 && rangeAnchorRow >= 0 && rangeEndRow >= 0 &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_C)) {
        const int r0 = std::min(rangeAnchorRow, rangeEndRow);
        const int r1 = std::max(rangeAnchorRow, rangeEndRow);
        const int c0 = std::min(rangeAnchorCol, rangeEndCol);
        const int c1 = std::max(rangeAnchorCol, rangeEndCol);

        // single cell: copy raw value (no csv quoting or trailing newline)
        if (r0 == r1 && c0 == c1 && r0 < static_cast<int>(data.size()) &&
            c0 < static_cast<int>(data[r0].size())) {
            const std::string raw = exportValue(data[r0][c0]);
            ImGui::SetClipboardText(raw.c_str());
        } else {
            std::string csv;
            for (int r = r0; r <= r1 && r < static_cast<int>(data.size()); ++r) {
                const auto& rowData = data[r];
                for (int c = c0; c <= c1; ++c) {
                    if (c > c0)
                        csv.push_back(',');
                    if (c < static_cast<int>(rowData.size()))
                        csv += csvEscape(exportValue(rowData[c]));
                }
                csv.push_back('\n');
            }
            if (!csv.empty())
                ImGui::SetClipboardText(csv.c_str());
        }
    }

    // Cmd/Ctrl+V → paste clipboard into the selected cell and enter edit mode
    if (config.allowEditing && editingRow == -1 && selectedRow >= 0 && selectedCol >= 0 &&
        selectedRow < static_cast<int>(data.size()) &&
        selectedCol < static_cast<int>(columns.size()) &&
        !config.nonEditableColumns.contains(selectedCol) &&
        (!cellEditableCb || cellEditableCb(selectedRow, selectedCol)) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_V)) {
        if (const char* clip = ImGui::GetClipboardText(); clip && *clip) {
            std::string val(clip);
            // strip a single trailing CRLF/LF (common when pasting our own csv copy)
            if (!val.empty() && val.back() == '\n')
                val.pop_back();
            if (!val.empty() && val.back() == '\r')
                val.pop_back();

            enterEditMode(selectedRow, selectedCol);
            if (editingRow == selectedRow && editingCol == selectedCol) {
                std::strncpy(editBuffer, val.c_str(), sizeof(editBuffer) - 1);
                editBuffer[sizeof(editBuffer) - 1] = '\0';
                initialCursorPos = static_cast<int>(strnlen(editBuffer, sizeof(editBuffer)));
                justEnteredEditWithChar = true;
                editOverlayFrames = 0;
            }
        }
    }

    handleKeyboardNavigation();

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
                        editOverlayFrames = 0;
                        io.InputQueueCharacters.Size = 0;
                    }
                }
            }
        }
    }

#if defined(__APPLE__) && DEARSQL_ENABLE_TABLE_AURORA
    ImVec2 tableMin(0, 0);
    ImVec2 tableMax(0, 0);
    bool tableRendered = false;
#endif

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
        // ensure the scroll target row is rendered even if it's outside the
        // currently visible range — otherwise the scroll-to-cell logic never runs.
        if (shouldScrollToCell && scrollTargetRow >= 0 &&
            scrollTargetRow < static_cast<int>(data.size())) {
            clipper.IncludeItemByIndex(scrollTargetRow);
        }

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
                        // use the table's inner clip rect + column MinX/MaxX so the
                        // visibility check reflects the actual scroll viewport, not the
                        // window content extent (which would span the whole table width
                        // and falsely report cells as visible/invisible).
                        ImGuiTable* tbl = ImGui::GetCurrentTable();
                        if (tbl) {
                            const int tableColIdx = config.showRowNumbers ? colIdx + 1 : colIdx;
                            const ImRect& clip = tbl->InnerClipRect;
                            const ImVec2 cellMin = ImGui::GetItemRectMin();
                            const ImVec2 cellMax = ImGui::GetItemRectMax();

                            // vertical: minimum scroll to bring the row into view
                            if (cellMin.y < clip.Min.y) {
                                ImGui::SetScrollY(ImGui::GetScrollY() + (cellMin.y - clip.Min.y));
                            } else if (cellMax.y > clip.Max.y) {
                                ImGui::SetScrollY(ImGui::GetScrollY() + (cellMax.y - clip.Max.y));
                            }

                            // horizontal: pull column bounds from the table directly so
                            // a clipped column still has valid coordinates.
                            if (tableColIdx >= 0 && tableColIdx < tbl->Columns.size()) {
                                const ImGuiTableColumn& tc = tbl->Columns[tableColIdx];
                                if (tc.MinX < clip.Min.x) {
                                    ImGui::SetScrollX(ImGui::GetScrollX() + (tc.MinX - clip.Min.x));
                                } else if (tc.MaxX > clip.Max.x) {
                                    ImGui::SetScrollX(ImGui::GetScrollX() + (tc.MaxX - clip.Max.x));
                                }
                            }
                        }

                        shouldScrollToCell = false;
                    }
                }
            }
        }

        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetStyle().ScrollbarSize);
        ImGui::EndTable();
#if defined(__APPLE__) && DEARSQL_ENABLE_TABLE_AURORA
        tableMin = ImGui::GetItemRectMin();
        tableMax = ImGui::GetItemRectMax();
        tableRendered = true;
#endif
    }

#if defined(__APPLE__) && DEARSQL_ENABLE_TABLE_AURORA
    if (tableRendered && tableMax.x > tableMin.x && tableMax.y > tableMin.y) {
        TableAurora::Params p{};
        p.x = tableMin.x;
        p.y = tableMin.y;
        p.w = tableMax.x - tableMin.x;
        p.h = tableMax.y - tableMin.y;
        p.r1 = colors.blue.x;
        p.g1 = colors.blue.y;
        p.b1 = colors.blue.z;
        p.r2 = colors.teal.x;
        p.g2 = colors.teal.y;
        p.b2 = colors.teal.z;
        p.time = static_cast<float>(ImGui::GetTime());
        p.intensity = 1.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCallback(TableAurora::callback, &p, sizeof(p));
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
#endif

    if (pendingEditOverlay) {
        renderEditOverlay();
        pendingEditOverlay = false;
    }

    if (releasedThisFrame) {
        suppressBoolToggleUntilMouseUp = false;
        suppressBoolToggleRow = -1;
        suppressBoolToggleCol = -1;
    }
}

void TableRenderer::renderCell(int row, int col) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const bool isPendingDelete =
        row < static_cast<int>(rowsPendingDelete.size()) && rowsPendingDelete[row];

    // Check if this cell is being edited
    if (config.allowEditing && editingRow == row && editingCol == col) {
        const ImVec4 tint = isPendingDelete
                                ? ImVec4(colors.red.x, colors.red.y, colors.red.z, 0.3f)
                                : ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.15f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(tint));

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
            // multiline or long values use the overlay popup; short single-line
            // values are edited inline within the cell. also check editBuffer so
            // a freshly pasted multiline/long value routes to the overlay too.
            const std::string& curr = data[row][col];
            const size_t bufLen = strnlen(editBuffer, sizeof(editBuffer));
            const bool useOverlay = curr.find('\n') != std::string::npos || curr.size() > 80 ||
                                    std::strchr(editBuffer, '\n') != nullptr || bufLen > 80;

            if (useOverlay) {
                editOverlayPosX = ImGui::GetCursorScreenPos().x;
                editOverlayPosY = ImGui::GetCursorScreenPos().y;
                editOverlayWidth = ImGui::GetColumnWidth();
                pendingEditOverlay = true;

                std::string firstLine = getFirstLine(curr);
                ImGui::Text("%s", firstLine.empty() ? " " : firstLine.c_str());
            } else {
                int extraFlags = 0;
                if (auto it = config.columnInputFlags.find(col);
                    it != config.columnInputFlags.end()) {
                    extraFlags = it->second;
                }

                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));

                if (editOverlayFrames < 3) {
                    ImGui::SetKeyboardFocusHere();
                    editOverlayFrames++;
                }

                const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | extraFlags;

                bool committed = false;
                if (justEnteredEditWithChar) {
                    committed = ImGui::InputText(
                        "##cell_inline", editBuffer, sizeof(editBuffer),
                        flags | ImGuiInputTextFlags_CallbackAlways,
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
                    committed =
                        ImGui::InputText("##cell_inline", editBuffer, sizeof(editBuffer), flags);
                }

                const bool inputActive = ImGui::IsItemActive();

                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);

                if (committed) {
                    exitEditMode(true);
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    exitEditMode(false);
                } else if (editOverlayFrames >= 3 && !inputActive) {
                    // commit on focus loss (clicked elsewhere)
                    exitEditMode(true);
                }
            }
        }
    } else {
        ImGui::PushID(row * static_cast<int>(columns.size()) + col);

        const bool isSelected = (selectedRow == row && selectedCol == col);
        const bool isEdited =
            (row < static_cast<int>(editedCells.size()) &&
             col < static_cast<int>(editedCells[row].size()) && editedCells[row][col]);

        const bool inRange = isInSelectionRange(row, col);
        if (isPendingDelete) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.red.x, colors.red.y, colors.red.z, 0.3f)));
        } else if (isEdited && isSelected) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.teal.x, colors.teal.y, colors.teal.z, 0.5f)));
        } else if (isEdited) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.teal.x, colors.teal.y, colors.teal.z, 0.3f)));
        } else if (isSelected) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface2));
        } else if (inRange) {
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_CellBg,
                ImGui::GetColorU32(ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.25f)));
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
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                float checkboxWidth = ImGui::GetFrameHeight();
                float columnWidth = ImGui::GetColumnWidth();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - checkboxWidth) * 0.5f);
                ImGui::BeginDisabled();
                ImGui::Checkbox("##bool", &checked);
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
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

        // prominent border around the selected cell so it stands out from the
        // subtle bg tint. clipped to the table's inner area so it doesn't bleed
        // past the scroll viewport when the cell is partially scrolled off.
        if (isSelected) {
            if (ImGuiTable* tbl = ImGui::GetCurrentTable()) {
                const int tableColIdx = config.showRowNumbers ? col + 1 : col;
                const ImRect cellRect = ImGui::TableGetCellBgRect(tbl, tableColIdx);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(tbl->InnerClipRect.Min, tbl->InnerClipRect.Max, true);
                dl->AddRect(cellRect.Min, cellRect.Max,
                            ImGui::GetColorU32(isPendingDelete ? colors.red : colors.blue), 0.0f, 0,
                            2.0f);
                dl->PopClipRect();
            }
        }

        ImGui::PopID();
    }
}

void TableRenderer::handleCellInteraction(int row, int col, bool isSelected) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const std::string& cellValue = data[row][col];
    const bool isNull = isNullSentinel(cellValue);
    const bool isBool = isBoolSentinel(cellValue);
    ImGuiIO& io = ImGui::GetIO();
    const bool canShiftSelect = io.KeyShift && selectedRow >= 0 && selectedCol >= 0;

    // boolean cells: render a small centered checkbox
    if (isBool) {
        bool checked = boolSentinelValue(cellValue);
        ImGuiTable* table = ImGui::GetCurrentTable();
        const int tableColIdx = config.showRowNumbers ? col + 1 : col;
        const ImRect cellRect = table ? ImGui::TableGetCellBgRect(table, tableColIdx) : ImRect();
        const int previousSelectedRow = selectedRow;
        const int previousSelectedCol = selectedCol;
        const bool shiftClickPressed = canShiftSelect && leftPressedThisFrame && table &&
                                       cellRect.Contains(ImGui::GetMousePos());
        if (shiftClickPressed) {
            setSelectionRange(previousSelectedRow, previousSelectedCol, row, col);
            suppressBoolToggleUntilMouseUp = true;
            suppressBoolToggleRow = row;
            suppressBoolToggleCol = col;
            if (onCellSelect)
                onCellSelect(row, col);
        }

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
        const bool suppressBoolToggle = suppressBoolToggleUntilMouseUp &&
                                        suppressBoolToggleRow == row &&
                                        suppressBoolToggleCol == col;
        // Read-only cells: dim normally to signal non-editable.
        // Shift-click suppression: still disable the click but keep full alpha
        // so the user doesn't see a flash on the cell they shift-clicked.
        const bool disableCheckbox = !editable || suppressBoolToggle;
        const bool transientDisable = suppressBoolToggle && editable;
        if (transientDisable)
            ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
        if (disableCheckbox)
            ImGui::BeginDisabled();

        if (ImGui::Checkbox("##bool", &checked)) {
            collapseSelectionToCell(row, col);
            if (onCellSelect)
                onCellSelect(row, col);
            if (onCellEdit) {
                onCellEdit(row, col,
                           std::string(checked ? BOOL_TRUE_SENTINEL : BOOL_FALSE_SENTINEL));
            }
        }

        if (disableCheckbox)
            ImGui::EndDisabled();
        if (transientDisable)
            ImGui::PopStyleVar();

        updateDragFromItem(row, col);

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        // select on click even if not toggling
        if (!suppressBoolToggle &&
            (ImGui::IsItemClicked() || ImGui::IsItemClicked(ImGuiMouseButton_Right))) {
            collapseSelectionToCell(row, col);
            if (onCellSelect)
                onCellSelect(row, col);
        }

        if (ImGui::IsItemHovered() && !isSelected && editable) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(colors.surface1));
        }

        renderCellContextMenu(row, col);
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

    const int previousSelectedRow = selectedRow;
    const int previousSelectedCol = selectedCol;
    if (ImGui::Selectable(displayText, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
        const bool shiftSelecting = canShiftSelect && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        if (shiftSelecting) {
            setSelectionRange(previousSelectedRow, previousSelectedCol, row, col);
        } else {
            collapseSelectionToCell(row, col);
        }

        if (onCellSelect) {
            onCellSelect(row, col);
        }

        if (!shiftSelecting && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (config.allowEditing) {
                enterEditMode(row, col);
            }

            if (onCellDoubleClick) {
                onCellDoubleClick(row, col);
            }
        }
    }

    updateDragFromItem(row, col);

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

    renderCellContextMenu(row, col);
}

void TableRenderer::copyCellToClipboard(int row, int col) const {
    if (row < 0 || row >= static_cast<int>(data.size()) || col < 0 ||
        col >= static_cast<int>(data[row].size())) {
        return;
    }

    const std::string raw = exportValue(data[row][col]);
    ImGui::SetClipboardText(raw.c_str());
}

void TableRenderer::copyRowToClipboard(int row) const {
    if (row < 0 || row >= static_cast<int>(data.size())) {
        return;
    }

    const auto& rowData = data[row];
    const int colCount =
        std::min(static_cast<int>(rowData.size()), static_cast<int>(columns.size()));
    std::string csv;
    for (int col = 0; col < colCount; ++col) {
        if (col > 0)
            csv.push_back(',');
        csv += csvEscape(exportValue(rowData[col]));
    }
    ImGui::SetClipboardText(csv.c_str());
}

void TableRenderer::renderCellContextMenu(int row, int col) {
    const std::string menuId = std::format("##cell_ctx_{}_{}", row, col);
    if (!ImGui::BeginPopupContextItem(menuId.c_str())) {
        return;
    }

    if (selectedRow != row || selectedCol != col) {
        collapseSelectionToCell(row, col);
        if (onCellSelect)
            onCellSelect(row, col);
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));

    if (paddedMenuItem(ICON_FA_COPY " Copy cell")) {
        copyCellToClipboard(row, col);
    }
    if (paddedMenuItem(ICON_FA_CLONE " Copy row")) {
        copyRowToClipboard(row);
    }

    const bool canFilter = onFilterByValue && row >= 0 && row < static_cast<int>(data.size()) &&
                           col >= 0 && col < static_cast<int>(data[row].size());
    if (paddedMenuItem(ICON_FA_FILTER " Filter by value", canFilter)) {
        onFilterByValue(row, col, data[row][col]);
    }

    ImGui::Separator();

    const bool canEdit = config.allowEditing && !config.nonEditableColumns.contains(col) &&
                         (!cellEditableCb || cellEditableCb(row, col));
    if (paddedMenuItem(ICON_FA_PEN_TO_SQUARE " Edit row", canEdit)) {
        enterEditMode(row, col);
    }

    ImGui::Separator();

    const bool canSetNull =
        config.allowEditing && onSetNull && columnNullableCb && columnNullableCb(col) && row >= 0 &&
        row < static_cast<int>(data.size()) && col >= 0 &&
        col < static_cast<int>(data[row].size()) && !isNullSentinel(data[row][col]) &&
        (!cellEditableCb || cellEditableCb(row, col));
    if (canSetNull && paddedMenuItem("Set NULL")) {
        onSetNull(row, col);
    }

    const bool canDelete = static_cast<bool>(onDeleteRow);
    if (paddedMenuItem(ICON_FA_TRASH_CAN " Delete row", canDelete)) {
        onDeleteRow(row);
    }

    ImGui::PopStyleVar();
    ImGui::EndPopup();
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

void TableRenderer::handleKeyboardNavigation() {
    if (selectedRow < 0 || selectedCol < 0 || data.empty() || columns.empty())
        return;
    if (editingRow != -1 || editingCol != -1)
        return;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    if (ImGui::GetIO().WantTextInput)
        return;

    const int maxRows = static_cast<int>(data.size());
    const int maxCols = static_cast<int>(columns.size());
    int newRow = selectedRow;
    int newCol = selectedCol;
    bool moved = false;

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && selectedRow > 0) {
        newRow = selectedRow - 1;
        moved = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && selectedRow < maxRows - 1) {
        newRow = selectedRow + 1;
        moved = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && selectedCol > 0) {
        newCol = selectedCol - 1;
        moved = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && selectedCol < maxCols - 1) {
        newCol = selectedCol + 1;
        moved = true;
    }

    if (moved) {
        collapseSelectionToCell(newRow, newCol);
        if (onCellSelect)
            onCellSelect(newRow, newCol);
        scrollToCell(newRow, newCol);
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

    // min width must fit the three button-with-badge widgets on one row
    float width = std::max(editOverlayWidth, 420.0f);
    int lineCount = 1;
    for (const char* p = editBuffer; *p; ++p) {
        if (*p == '\n')
            lineCount++;
    }
    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    float lines = std::min(std::max(static_cast<float>(lineCount + 1), 5.0f), 15.0f);
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

        // word-wrap so long lines stay visible without horizontal scrolling
        const ImGuiInputTextFlags inputBaseFlags = ImGuiInputTextFlags_WordWrap | extraFlags;

        if (justEnteredEditWithChar) {
            ImGui::InputTextMultiline(
                "##edit_overlay_input", editBuffer, sizeof(editBuffer), ImVec2(-FLT_MIN, inputH),
                inputBaseFlags | ImGuiInputTextFlags_CallbackAlways,
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
                                      ImVec2(-FLT_MIN, inputH), inputBaseFlags);
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
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    if (ImGui::SmallButton(ICON_FA_CHEVRON_DOWN)) {
        ImGui::OpenPopup(popupId.c_str());
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
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
    ImGui::PopStyleVar(2);

    ImGui::PopID();
}
