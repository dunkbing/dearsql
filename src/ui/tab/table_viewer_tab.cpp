#include "ui/tab/table_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/query_history.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <future>
#include <iostream>
#include <utility>

TableViewerTab::TableViewerTab(const std::string& name, std::string databasePath,
                               std::string tableName, IDatabaseNode* node)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)),
      tableName(std::move(tableName)), node_(node) {
    initializeTableRenderer();
    initializeFilterAutoComplete();
    loadDataAsync();
}

void TableViewerTab::render() {
    const auto& colors = Application::getInstance().getCurrentColors();

    checkAsyncLoadStatus();

    ImGui::Text("Table: %s", tableName.c_str());
    ImGui::Separator();

    // Style for buttons and inputs in this tab
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);

    // Filter input with auto-completion
    ImGui::AlignTextToFramePadding(); // Center the label vertically with the input field
    ImGui::Text("Filters:");
    ImGui::SameLine();

    // Use the AutoCompleteInput component
    if (filterAutoComplete &&
        filterAutoComplete->render("##filter", filterBuffer, sizeof(filterBuffer))) {
        applyFilter();
    }

    // Apply Filter button with green tint
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.green.x * 0.3f, colors.green.y * 0.3f,
                                                  colors.green.z * 0.3f, 1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        ImVec4(colors.green.x * 0.5f, colors.green.y * 0.5f, colors.green.z * 0.5f, 1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        ImVec4(colors.green.x * 0.7f, colors.green.y * 0.7f, colors.green.z * 0.7f, 1.0f));
    if (ImGui::Button("Apply Filter")) {
        applyFilter();
    }
    ImGui::PopStyleColor(3);

    // Clear Filter button with red tint
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.red.x * 0.3f, colors.red.y * 0.3f,
                                                  colors.red.z * 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(colors.red.x * 0.5f, colors.red.y * 0.5f,
                                                         colors.red.z * 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(colors.red.x * 0.7f, colors.red.y * 0.7f,
                                                        colors.red.z * 0.7f, 1.0f));
    if (ImGui::Button("Clear Filter")) {
        memset(filterBuffer, 0, sizeof(filterBuffer));
        if (filterAutoComplete) {
            filterAutoComplete->hideAutoComplete();
        }
        if (!currentFilter.empty()) {
            Logger::debug("Clearing filter for table: " + tableName);
            // Clear the filter FIRST, then reload
            currentFilter.clear();
            filterChanged = true;
            // Reset to first page when filter is cleared
            currentPage = 0;
            // Clear selection when filter changes
            selectedRow = -1;
            selectedCol = -1;
            // Clear any error states
            hasLoadingError = false;
            loadingError.clear();
            // Clear any existing table data to force fresh load
            tableData.clear();
            columnNames.clear();
            totalRows = 0;
            // Reload data without filter
            loadDataAsync();
        }
    }
    ImGui::PopStyleColor(3);

    // Show current filter if active
    if (!currentFilter.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "Active filter: %s",
                           currentFilter.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(%d rows)", totalRows);
    }

    // Pop button/input styles
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar();

    // Show loading error if any
    if (hasLoadingError) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("Error loading data: %s", loadingError.c_str());
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            const std::string errorText = "Error loading data: " + loadingError;
            ImGui::SetClipboardText(errorText.c_str());
        }
    }

    // Reserve space for bottom controls (pagination + action buttons)
    const float bottomControlsHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    const float availableHeight = ImGui::GetContentRegionAvail().y - bottomControlsHeight;

    // Horizontal layout: table area | panel content (when open) | toggle strip (always)
    const float toggleStripWidth = 28.0f;
    const float panelContentWidth = rightPanelOpen ? rightPanelWidth : 0.0f;
    const float totalAvailableWidth = ImGui::GetContentRegionAvail().x;
    float tableAreaWidth = totalAvailableWidth - toggleStripWidth - panelContentWidth;
    tableAreaWidth = std::max(200.0f, tableAreaWidth);

    // Table display in a child window to prevent cutoff
    if (ImGui::BeginChild("TableArea", ImVec2(tableAreaWidth, availableHeight),
                          ImGuiChildFlags_None)) {
        if (isLoadingData) {
            ImGui::Text("Loading table data...");
        } else if (!columnNames.empty() && !tableData.empty()) {
            // Update table renderer with current data
            tableRenderer->setColumns(columnNames);
            tableRenderer->setData(tableData);
            tableRenderer->setCellEditedStatus(editedCells);
            tableRenderer->setSelectedCell(selectedRow, selectedCol);
            tableRenderer->setRowNumberOffset(currentPage * rowsPerPage);
            tableRenderer->setSortColumn(sortColumn, sortDirection);

            tableRenderer->render("TableData");

            // Handle keyboard navigation - skip when editing a cell so arrow keys work in the input
            if (selectedRow >= 0 && selectedCol >= 0 && !tableRenderer->isEditing()) {
                handleKeyboardNavigation();
            }
        } else {
            if (!currentFilter.empty()) {
                ImGui::Text("No rows match the filter: %s", currentFilter.c_str());
                ImGui::TextColored(
                    ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    "Try a different filter condition or click 'Clear Filter' to see all data.");
            } else if (hasLoadingError) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error loading data");
            } else {
                ImGui::Text("No data to display. Execute a query to see results here.");
            }
        }
    }
    ImGui::EndChild();

    // Panel content (when open)
    if (rightPanelOpen) {
        ImGui::SameLine(0, 0);
        renderRightPanel(panelContentWidth, availableHeight);
    }

    // Toggle strip on the far right (always visible)
    ImGui::SameLine(0, 0);
    renderRightPanelToggleStrip(toggleStripWidth, availableHeight);

    // Pagination controls at the bottom
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);

    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;

    if (ImGui::Button("<<") && currentPage > 0) {
        firstPage();
    }
    ImGui::SameLine();

    if (ImGui::Button("<") && currentPage > 0) {
        previousPage();
    }
    ImGui::SameLine(0, Theme::Spacing::M);

    ImGui::Text("Page %d of %d (%d rows total)", currentPage + 1, totalPages, totalRows);
    ImGui::SameLine(0, Theme::Spacing::M);

    if (ImGui::Button(">") && currentPage < totalPages - 1) {
        nextPage();
    }
    ImGui::SameLine();

    if (ImGui::Button(">>") && currentPage < totalPages - 1) {
        lastPage();
    }

    // Page size selector
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20, 0)); // Add some spacing
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Rows per page:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);

    static const int pageSizeOptions[] = {10, 25, 50, 100, 200, 500};
    static const char* pageSizeLabels[] = {"10", "25", "50", "100", "200", "500"};
    int currentSizeIndex = -1;

    // Find current size in options
    for (int i = 0; i < IM_ARRAYSIZE(pageSizeOptions); i++) {
        if (pageSizeOptions[i] == rowsPerPage) {
            currentSizeIndex = i;
            break;
        }
    }

    // Store the string to avoid dangling pointer
    std::string customSizeLabel;
    const char* currentSizeLabel;
    if (currentSizeIndex >= 0) {
        currentSizeLabel = pageSizeLabels[currentSizeIndex];
    } else {
        customSizeLabel = std::to_string(rowsPerPage);
        currentSizeLabel = customSizeLabel.c_str();
    }

    if (ImGui::BeginCombo("##pagesize", currentSizeLabel)) {
        for (int i = 0; i < IM_ARRAYSIZE(pageSizeOptions); i++) {
            const bool isSelected = (pageSizeOptions[i] == rowsPerPage);
            if (ImGui::Selectable(pageSizeLabels[i], isSelected)) {
                if (pageSizeOptions[i] != rowsPerPage) {
                    // Calculate first row index with old page size
                    const int oldRowsPerPage = rowsPerPage;
                    const int firstRowOnCurrentPage = currentPage * oldRowsPerPage;

                    // Update to new page size
                    rowsPerPage = pageSizeOptions[i];

                    // Calculate new page to stay on approximately the same data
                    currentPage = firstRowOnCurrentPage / rowsPerPage;

                    // Reload data with new page size
                    loadDataAsync();
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Action buttons
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20, 0)); // Add some spacing
    ImGui::SameLine();

    // Refresh button with blue color
    ImGui::PushStyleColor(ImGuiCol_Text, colors.blue);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) {
        refreshData();
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Refresh");
    }

    // Show loading indicator
    if (isLoadingData) {
        ImGui::SameLine();
        ImGui::Text("Loading...");
    }
    ImGui::SameLine();

    if (hasChanges) {
        // Save button with green color when enabled
        ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK)) {
            saveChanges();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Save");
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_FLOPPY_DISK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Save");
        }
    }

    ImGui::SameLine();
    if (hasChanges) {
        // Cancel button with red color when enabled
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        if (ImGui::Button(ICON_FA_XMARK)) {
            cancelChanges();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reject changes");
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_XMARK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Reject changes");
        }
    }

    // Add row button
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS)) {
        addRow();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add row");
    }

    if (hasChanges) {
        ImGui::SameLine();
        ImGui::TextColored(colors.peach, "Unsaved changes");
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    // Check async SQL execution status
    checkSQLExecutionStatus();

    // Show save confirmation dialog if needed
    showSaveConfirmationDialog();
}

void TableViewerTab::nextPage() {
    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    if (currentPage < totalPages - 1 && !isLoadingData) {
        currentPage++;
        // When moving to next page from keyboard navigation, select first row
        if (selectedRow >= 0 && selectedCol >= 0) {
            selectedRow = 0; // Select first row of new page
        }
        loadDataAsync();
    }
}

void TableViewerTab::previousPage() {
    if (currentPage > 0 && !isLoadingData) {
        currentPage--;
        // When moving to previous page from keyboard navigation, select last row
        if (selectedRow >= 0 && selectedCol >= 0) {
            // We'll set this to the last row after data loads
            selectedRow =
                rowsPerPage - 1; // This will be adjusted in checkAsyncLoadStatus if needed
        }
        loadDataAsync();
    }
}

void TableViewerTab::firstPage() {
    if (!isLoadingData) {
        currentPage = 0;
        loadDataAsync();
    }
}

void TableViewerTab::lastPage() {
    if (isLoadingData)
        return;

    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    currentPage = totalPages - 1;
    loadDataAsync();
}

void TableViewerTab::refreshData() {
    if (!isLoadingData) {
        // Reset selection state
        selectedRow = -1;
        selectedCol = -1;
        hasChanges = false;
        hasLoadingError = false;
        loadingError.clear();

        // Clear edited cells tracking
        for (auto& row : editedCells) {
            std::fill(row.begin(), row.end(), false);
        }

        loadDataAsync();
    }
}

void TableViewerTab::saveChanges() {
    if (!hasChanges) {
        return;
    }

    // Generate SQL statements for the changes
    pendingUpdateSQL = generateUpdateSQL();

    if (pendingUpdateSQL.empty()) {
        // No valid SQL generated, just clear changes
        hasChanges = false;
        for (auto& row : editedCells) {
            std::fill(row.begin(), row.end(), false);
        }
        return;
    }

    // Show confirmation dialog
    showSaveDialog = true;
}

void TableViewerTab::cancelChanges() {
    // Remove newly added rows (iterate backwards to preserve indices)
    for (int i = static_cast<int>(isNewRow.size()) - 1; i >= 0; i--) {
        if (isNewRow[i]) {
            tableData.erase(tableData.begin() + i);
            originalData.erase(originalData.begin() + i);
            editedCells.erase(editedCells.begin() + i);
            isNewRow.erase(isNewRow.begin() + i);
        }
    }

    // Restore original data for remaining rows
    tableData = originalData;
    hasChanges = false;

    // Clear edited cells tracking
    for (auto& row : editedCells) {
        std::fill(row.begin(), row.end(), false);
    }

    // Reset selection state
    selectedRow = -1;
    selectedCol = -1;
}

void TableViewerTab::addRow() {
    if (columnNames.empty())
        return;

    // Insert below selected row, or at end
    int insertIdx = (selectedRow >= 0) ? selectedRow + 1 : static_cast<int>(tableData.size());

    // Create empty row
    std::vector<std::string> newRow(columnNames.size(), "");

    tableData.insert(tableData.begin() + insertIdx, newRow);
    originalData.insert(originalData.begin() + insertIdx,
                        std::vector<std::string>(columnNames.size(), ""));
    editedCells.insert(editedCells.begin() + insertIdx,
                       std::vector<bool>(columnNames.size(), true));
    isNewRow.insert(isNewRow.begin() + insertIdx, true);

    hasChanges = true;

    // Select first cell of new row
    selectedRow = insertIdx;
    selectedCol = 0;
}

void TableViewerTab::selectCell(const int row, const int col) {
    const int tableSize = static_cast<int>(tableData.size());
    const int totalCols = static_cast<int>(columnNames.size());
    if (row >= 0 && row < tableSize && col >= 0 && col < totalCols) {
        selectedRow = row;
        selectedCol = col;
    }
}

void TableViewerTab::handleKeyboardNavigation() {
    // Basic validation
    if (selectedRow < 0 || selectedCol < 0 || tableData.empty() || columnNames.empty()) {
        return;
    }

    // Only handle keyboard input if this window/tab is focused
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        return;
    }

    const int maxRows = static_cast<int>(tableData.size());
    const int maxCols = static_cast<int>(columnNames.size());

    int newRow = selectedRow;
    int newCol = selectedCol;
    bool moved = false;

    // Handle arrow key navigation - try both with and without repeat
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
        if (selectedRow > 0) {
            newRow = selectedRow - 1;
            moved = true;
        } else if (currentPage > 0) {
            previousPage();
            return;
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
               ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
        if (selectedRow < maxRows - 1) {
            newRow = selectedRow + 1;
            moved = true;
        } else {
            const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
            if (currentPage < totalPages - 1) {
                nextPage();
                return;
            }
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
               ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
        if (selectedCol > 0) {
            newCol = selectedCol - 1;
            moved = true;
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
               ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        if (selectedCol < maxCols - 1) {
            newCol = selectedCol + 1;
            moved = true;
        }
    }

    // Update selection if we moved
    if (moved) {
        selectCell(newRow, newCol);

        // Scroll to the new cell to keep it visible
        if (tableRenderer) {
            tableRenderer->scrollToCell(newRow, newCol);
        }
    }
}

void TableViewerTab::loadDataAsync() {
    isLoadingData = true;
    hasLoadingError = false;
    loadingError.clear();

    // Clear current data to show loading state if filter changed
    if (filterChanged) {
        tableData.clear();
        columnNames.clear();
        totalRows = 0;
        filterChanged = false;
        Logger::debug("Cleared previous filtered data, starting fresh load");
    }

    // Build ORDER BY clause from sort state
    std::string orderByClause;
    if (sortColumn >= 0 && sortDirection != SortDirection::None && !sortColumnName.empty()) {
        orderByClause = std::format("\"{}\" {}", sortColumnName,
                                    sortDirection == SortDirection::Ascending ? "ASC" : "DESC");
    }

    // Launch async loading
    dataLoadFuture = std::async(std::launch::async, [this, orderByClause]() {
        try {
            totalRows = node_->getRowCount(tableName, currentFilter);
            columnNames = node_->getColumnNames(tableName);
            const int offset = currentPage * rowsPerPage;
            tableData =
                node_->getTableData(tableName, rowsPerPage, offset, currentFilter, orderByClause);

            // Store original data for change tracking
            originalData = tableData;
            hasChanges = false;

            // Initialize edited cells and new row tracking
            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(columnNames.size(), false));
            isNewRow = std::vector<bool>(tableData.size(), false);
        } catch (const std::exception& e) {
            hasLoadingError = true;
            loadingError = e.what();
        }
    });
}

void TableViewerTab::checkAsyncLoadStatus() {
    if (dataLoadFuture.valid() &&
        dataLoadFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        dataLoadFuture.get(); // Wait for completion and handle any exceptions
        isLoadingData = false;

        // Auto-select first cell on initial load
        if (!initialSelectionDone && !tableData.empty() && !columnNames.empty()) {
            selectedRow = 0;
            selectedCol = 0;
            initialSelectionDone = true;
        }

        // Add to query history if load was successful
        if (!hasLoadingError && !tableData.empty()) {
            const int offset = currentPage * rowsPerPage;
            std::string query = std::format("SELECT * FROM {}", tableName);
            if (!currentFilter.empty()) {
                query += std::format(" WHERE {}", currentFilter);
            }
            if (sortColumn >= 0 && sortDirection != SortDirection::None &&
                !sortColumnName.empty()) {
                query += std::format(" ORDER BY \"{}\" {}", sortColumnName,
                                     sortDirection == SortDirection::Ascending ? "ASC" : "DESC");
            }
            query += std::format(" LIMIT {} OFFSET {}", rowsPerPage, offset);

            QueryHistory::instance().add(query, static_cast<int>(tableData.size()));
        }
    }
}

std::vector<std::string> TableViewerTab::getPrimaryKeyColumns() const {
    // Find table columns in node (check both tables and views)
    for (const auto& table : node_->getTables()) {
        bool matches = (table.name == tableName) || (table.fullName.ends_with("." + tableName));
        if (matches) {
            std::vector<std::string> pkColumns;
            for (const auto& column : table.columns) {
                if (column.isPrimaryKey) {
                    pkColumns.push_back(column.name);
                }
            }
            return pkColumns;
        }
    }

    // Check views as well
    for (const auto& view : node_->getViews()) {
        bool matches = (view.name == tableName) || (view.fullName.ends_with("." + tableName));
        if (matches) {
            std::vector<std::string> pkColumns;
            for (const auto& column : view.columns) {
                if (column.isPrimaryKey) {
                    pkColumns.push_back(column.name);
                }
            }
            return pkColumns;
        }
    }

    return {};
}

std::vector<std::string> TableViewerTab::generateUpdateSQL() {
    std::vector<std::string> sqlStatements;

    const std::vector<std::string> pkColumns = getPrimaryKeyColumns();

    std::cout << "Generating UPDATE SQL for table: " << tableName << std::endl;
    std::cout << "Primary key columns: ";
    for (const auto& pk : pkColumns) {
        std::cout << pk << " ";
    }
    std::cout << std::endl;

    // Build table reference once
    std::string tableRef;
    if (const auto dotPos = tableName.find('.'); dotPos != std::string::npos) {
        const std::string schemaName = tableName.substr(0, dotPos);
        const std::string tableNameOnly = tableName.substr(dotPos + 1);
        tableRef = std::format(R"("{}"."{}")", schemaName, tableNameOnly);
    } else {
        tableRef = std::format(R"("{}")", tableName);
    }

    // Process each edited cell
    for (int rowIdx = 0; rowIdx < editedCells.size(); rowIdx++) {
        // Generate INSERT for newly added rows
        if (rowIdx < static_cast<int>(isNewRow.size()) && isNewRow[rowIdx]) {
            std::string cols;
            std::string vals;
            for (int colIdx = 0; colIdx < static_cast<int>(columnNames.size()); colIdx++) {
                if (colIdx > 0) {
                    cols += ", ";
                    vals += ", ";
                }
                cols += std::format(R"("{}")", columnNames[colIdx]);
                const std::string& val = tableData[rowIdx][colIdx];
                if (val.empty() || val == "NULL") {
                    vals += "NULL";
                } else {
                    vals += "'" + val + "'";
                }
            }
            sqlStatements.push_back(
                std::format("INSERT INTO {} ({}) VALUES ({});", tableRef, cols, vals));
            continue;
        }

        for (int colIdx = 0; colIdx < editedCells[rowIdx].size(); colIdx++) {
            if (!editedCells[rowIdx][colIdx]) {
                continue; // Cell not edited
            }

            const std::string& columnName = columnNames[colIdx];
            const std::string& newValue = tableData[rowIdx][colIdx];

            // Build UPDATE statement
            std::string sql = std::format(R"(UPDATE {} SET "{}" = )", tableRef, columnName);

            // Add quoted value
            if (newValue == "NULL") {
                sql += "NULL";
            } else {
                sql += "'" + newValue + "'"; // Simple escaping - could be improved
            }

            sql += " WHERE ";

            // Build WHERE clause
            std::vector<std::string> whereConditions;

            if (!pkColumns.empty()) {
                // Use primary key columns
                for (const auto& pkCol : pkColumns) {
                    auto pkColIt = std::ranges::find(columnNames, pkCol);
                    if (pkColIt != columnNames.end()) {
                        const int pkColIdx =
                            static_cast<int>(std::distance(columnNames.begin(), pkColIt));
                        const std::string& pkValue = originalData[rowIdx][pkColIdx];
                        whereConditions.push_back(std::format("\"{}\" = '{}'", pkCol, pkValue));
                    }
                }
            } else {
                // For Postgres without primary key, use all columns as condition
                for (int condColIdx = 0; condColIdx < columnNames.size(); condColIdx++) {
                    const std::string& condValue = originalData[rowIdx][condColIdx];
                    if (condValue == "NULL") {
                        whereConditions.push_back(
                            std::format("\"{}\" IS NULL", columnNames[condColIdx]));
                    } else {
                        whereConditions.push_back(
                            std::format("\"{}\" = '{}'", columnNames[condColIdx], condValue));
                    }
                }
            }

            // Join conditions with AND
            for (int i = 0; i < whereConditions.size(); i++) {
                sql += whereConditions[i];
                if (i < whereConditions.size() - 1) {
                    sql += " AND ";
                }
            }

            sql += ";";
            sqlStatements.push_back(sql);
        }
    }

    return sqlStatements;
}

void TableViewerTab::showSaveConfirmationDialog() {
    if (!showSaveDialog) {
        return;
    }

    // Only open popup once when dialog first shows
    if (!dialogOpened) {
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        ImGui::OpenPopup("Confirm Save Changes");
        dialogOpened = true;
    }

    if (ImGui::BeginPopupModal("Confirm Save Changes", nullptr)) {
        ImGui::Text("The following SQL statements will be executed:");
        ImGui::Separator();

        // Show SQL statements in a scrollable area
        if (ImGui::BeginChild("SQLPreview", ImVec2(0, -50), true)) {
            for (int i = 0; i < pendingUpdateSQL.size(); i++) {
                ImGui::Text("%d.", i + 1);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", pendingUpdateSQL[i].c_str());
                if (i < pendingUpdateSQL.size() - 1) {
                    ImGui::Separator();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Buttons
        if (executingSQL) {
            // Show spinner and disable buttons during execution
            ImGui::BeginDisabled();
            ImGui::Button("Execute", ImVec2(120, 0));
            ImGui::EndDisabled();

            ImGui::SameLine();
            const auto& colors = Application::getInstance().getCurrentColors();
            UIUtils::Spinner("##spinner", 8.0f, 2, ImGui::GetColorU32(colors.blue));

            ImGui::SameLine();
            ImGui::Text("Executing...");
        } else {
            if (ImGui::Button("Execute", ImVec2(120, 0))) {
                executingSQL = true;

                auto sqlStatements = pendingUpdateSQL;
                IDatabaseNode* executor = node_;

                sqlExecutionFuture = std::async(
                    std::launch::async,
                    [executor, sqlStatements]() -> std::pair<bool, std::string> {
                        if (!executor) {
                            return std::make_pair(
                                false, "Error: Database does not support query execution");
                        }

                        bool allSuccess = true;
                        std::string errorMessage;

                        for (const auto& sql : sqlStatements) {
                            std::cout << "Executing SQL: " << sql << std::endl;
                            const auto results = executor->executeQuery(sql);
                            const auto& r = results.empty() ? QueryResult{} : results.back();
                            std::cout << "SQL Result: " << (r.success ? r.message : r.errorMessage)
                                      << std::endl;

                            if (!r.success) {
                                allSuccess = false;
                                errorMessage = "Error: " + r.errorMessage;
                                std::cerr << "SQL execution failed: " << r.errorMessage
                                          << std::endl;
                                return std::make_pair(allSuccess, errorMessage);
                            }
                        }

                        auto result = std::make_pair(allSuccess, errorMessage);

                        if (result.first) {
                            std::cout << "All SQL statements executed successfully" << std::endl;
                        }

                        return result;
                    });
            }
        }

        ImGui::SameLine();
        if (executingSQL) {
            ImGui::BeginDisabled();
            ImGui::Button("Cancel", ImVec2(120, 0));
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                showSaveDialog = false;
                pendingUpdateSQL.clear();
                dialogOpened = false;
            }
        }

        ImGui::EndPopup();
    } else if (!ImGui::IsPopupOpen("Confirm Save Changes")) {
        // Dialog was closed by clicking outside or ESC
        showSaveDialog = false;
        pendingUpdateSQL.clear();
        dialogOpened = false;
    }
}

void TableViewerTab::checkSQLExecutionStatus() {
    if (!executingSQL || !sqlExecutionFuture.valid()) {
        return;
    }

    // Check if async execution is complete
    if (sqlExecutionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto [success, errorMessage] = sqlExecutionFuture.get();

            if (success) {
                // Mark changes as saved
                hasChanges = false;
                originalData = tableData;
                for (auto& row : editedCells) {
                    std::fill(row.begin(), row.end(), false);
                }
            } else {
                std::cerr << "Failed to execute SQL statements: " << errorMessage << std::endl;
                // Keep the dialog open but reset execution state
            }

            // Reset execution state
            executingSQL = false;

            // Close dialog on successful execution, keep open on error for user to see
            if (success) {
                showSaveDialog = false;
                pendingUpdateSQL.clear();
                dialogOpened = false;
            }

        } catch (const std::exception& e) {
            std::cerr << "Exception during SQL execution: " << e.what() << std::endl;
            executingSQL = false;
        }
    }
}

void TableViewerTab::applyFilter() {
    std::string newFilter = std::string(filterBuffer);

    // Trim whitespace
    newFilter.erase(0, newFilter.find_first_not_of(" \t\n\r"));
    newFilter.erase(newFilter.find_last_not_of(" \t\n\r") + 1);

    // Check if filter actually changed
    if (newFilter == currentFilter) {
        return;
    }

    currentFilter = newFilter;
    filterChanged = true;

    // Reset to first page when filter changes
    currentPage = 0;

    // Clear selection when filter changes
    selectedRow = -1;
    selectedCol = -1;

    // Clear any error states
    hasLoadingError = false;
    loadingError.clear();

    // Reload data with new filter
    loadDataAsync();
}

void TableViewerTab::initializeTableRenderer() {
    // Initialize table renderer with editable configuration
    TableRenderer::Config config;
    config.allowEditing = true;
    config.allowSelection = true;
    config.showRowNumbers = true;
    config.minHeight = 200.0f;

    tableRenderer = std::make_unique<TableRenderer>(config);

    // Set up callbacks
    tableRenderer->setOnCellEdit([this](int row, int col, const std::string& newValue) {
        if (newValue != tableData[row][col]) {
            tableData[row][col] = newValue;
            hasChanges = true;

            // Mark cell as edited
            if (row < editedCells.size() && col < editedCells[row].size()) {
                editedCells[row][col] = true;
            }
        }
    });

    tableRenderer->setOnCellSelect([this](int row, int col) { selectCell(row, col); });

    // Sort callback
    tableRenderer->setOnSortChanged(
        [this](int col, const std::string& colName, SortDirection direction) {
            sortColumn = col;
            sortColumnName = colName;
            sortDirection = direction;

            // Reset to first page when sort changes
            currentPage = 0;

            // Clear selection
            selectedRow = -1;
            selectedCol = -1;

            // Reload data with new sort
            loadDataAsync();
        });
}

void TableViewerTab::initializeFilterAutoComplete() {
    AutoCompleteInput::Config config;
    config.hint = "e.g. id = 1 and name LIKE 'john%'";
    config.width = 400.0f;
    config.onSubmit = [this]() { applyFilter(); };

    // Initialize with SQL keywords
    config.keywords = {"AND", "OR",  "NOT",  "IN",   "LIKE",  "BETWEEN", "IS",   "NULL",  "EXISTS",
                       "ALL", "ANY", "SOME", "TRUE", "FALSE", "ASC",     "DESC", "LIMIT", "OFFSET"};

    // Add column names when they become available
    if (!columnNames.empty()) {
        config.keywords.insert(config.keywords.end(), columnNames.begin(), columnNames.end());
    }

    filterAutoComplete = std::make_unique<AutoCompleteInput>(config);
}

void TableViewerTab::renderRightPanelToggleStrip(float stripWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("PanelToggleStrip", ImVec2(stripWidth, availableHeight),
                          ImGuiChildFlags_None)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        // Draw left border line for the strip
        drawList->AddLine(stripPos, ImVec2(stripPos.x, stripPos.y + availableHeight),
                          ImGui::GetColorU32(colors.overlay0), 1.0f);

        // "Panels" rotated label as a compact clickable tab at the top
        const char* label = "Panels";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const float padding = 6.0f;
        // After rotation, text width becomes button height, text height becomes button width
        const float buttonW = stripWidth;
        const float buttonH = textSize.x + padding * 2.0f;

        // Clickable area for the "Panels" button
        ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y));
        ImGui::InvisibleButton("##togglePanel", ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            rightPanelOpen = !rightPanelOpen;
        }

        // Button background
        const ImVec2 btnMin = stripPos;
        const ImVec2 btnMax(stripPos.x + buttonW, stripPos.y + buttonH);
        if (rightPanelOpen) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        } else if (hovered) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        }

        // Bottom border of the button area
        drawList->AddLine(ImVec2(btnMin.x, btnMax.y), btnMax, ImGui::GetColorU32(colors.overlay0),
                          1.0f);

        // Draw rotated text centered in the button area
        // Push full-screen clip rect so text isn't clipped before rotation
        const float cx = stripPos.x + buttonW * 0.5f;
        const float cy = stripPos.y + buttonH * 0.5f;
        const float textX = cx - textSize.x * 0.5f;
        const float textY = cy - textSize.y * 0.5f;

        drawList->PushClipRectFullScreen();
        const int vtxBegin = drawList->VtxBuffer.Size;
        drawList->AddText(ImVec2(textX, textY),
                          ImGui::GetColorU32(hovered ? colors.text : colors.subtext0), label);
        const int vtxEnd = drawList->VtxBuffer.Size;

        // Rotate all text vertices 90 degrees (top-to-bottom reading) around center
        for (int i = vtxBegin; i < vtxEnd; i++) {
            ImDrawVert& v = drawList->VtxBuffer[i];
            const float dx = v.pos.x - cx;
            const float dy = v.pos.y - cy;
            v.pos.x = cx - dy;
            v.pos.y = cy + dx;
        }
        drawList->PopClipRect();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void TableViewerTab::renderRightPanel(float panelWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginChild("RightPanel", ImVec2(panelWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        // Resize handle on the left edge of the panel
        {
            const float handleWidth = 4.0f;
            const ImVec2 panelPos = ImGui::GetWindowPos();
            const ImVec2 handleMin(panelPos.x, panelPos.y);
            const ImVec2 handleMax(panelPos.x + handleWidth, panelPos.y + availableHeight);

            ImGui::SetCursorScreenPos(handleMin);
            ImGui::InvisibleButton("##resizeHandle", ImVec2(handleWidth, availableHeight));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                rightPanelWidth -= ImGui::GetIO().MouseDelta.x;
                rightPanelWidth = std::clamp(rightPanelWidth, 200.0f, 600.0f);
            }

            // Reset cursor to after the handle for normal content
            ImGui::SetCursorPos(ImVec2(0, 0));
        }

        if (ImGui::BeginTabBar("##PanelTabs")) {
            if (ImGui::BeginTabItem("Value")) {
                activeRightPanelTab = 0;
                renderValueTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Metadata")) {
                activeRightPanelTab = 1;
                renderMetadataTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void TableViewerTab::syncValuePanelBuffer() {
    // Check if selection changed
    bool selectionChanged = (selectedRow != lastSyncedRow || selectedCol != lastSyncedCol);

    // Also detect if the cell value was edited externally (e.g., inline editing in table)
    bool valueChanged = false;
    if (!selectionChanged && selectedRow >= 0 && selectedCol >= 0 &&
        selectedRow < static_cast<int>(tableData.size()) &&
        selectedCol < static_cast<int>(columnNames.size())) {
        const std::string& currentValue = tableData[selectedRow][selectedCol];
        if (!valuePanelBufferDirty && std::string(valuePanelBuffer) != currentValue) {
            valueChanged = true;
        }
    }

    if (selectionChanged || valueChanged) {
        lastSyncedRow = selectedRow;
        lastSyncedCol = selectedCol;
        valuePanelBufferDirty = false;

        if (selectedRow >= 0 && selectedCol >= 0 &&
            selectedRow < static_cast<int>(tableData.size()) &&
            selectedCol < static_cast<int>(columnNames.size())) {
            const std::string& value = tableData[selectedRow][selectedCol];
            std::strncpy(valuePanelBuffer, value.c_str(), sizeof(valuePanelBuffer) - 1);
            valuePanelBuffer[sizeof(valuePanelBuffer) - 1] = '\0';
        } else {
            valuePanelBuffer[0] = '\0';
        }
    }
}

void TableViewerTab::renderValueTab() {
    const auto& colors = Application::getInstance().getCurrentColors();

    syncValuePanelBuffer();

    if (selectedRow < 0 || selectedCol < 0 || selectedRow >= static_cast<int>(tableData.size()) ||
        selectedCol >= static_cast<int>(columnNames.size())) {
        ImGui::TextColored(colors.subtext0, "Select a cell to view its value.");
        return;
    }

    // Header: column name and row info
    ImGui::TextColored(colors.blue, "%s", columnNames[selectedCol].c_str());
    ImGui::SameLine();
    ImGui::TextColored(colors.subtext0, "(Row %d)", currentPage * rowsPerPage + selectedRow + 1);
    ImGui::Separator();

    // Multiline text editor for the cell value
    const float availH = ImGui::GetContentRegionAvail().y -
                         (valuePanelBufferDirty ? ImGui::GetFrameHeightWithSpacing() + 4.0f : 0.0f);
    if (ImGui::InputTextMultiline("##valuePanelEdit", valuePanelBuffer, sizeof(valuePanelBuffer),
                                  ImVec2(-1, availH))) {
        // Check if user modified the buffer
        const std::string& currentValue = tableData[selectedRow][selectedCol];
        valuePanelBufferDirty = (std::string(valuePanelBuffer) != currentValue);
    }

    // Apply / Revert buttons when buffer is dirty
    if (valuePanelBufferDirty) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.green.x * 0.3f, colors.green.y * 0.3f,
                                                      colors.green.z * 0.3f, 1.0f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(colors.green.x * 0.5f, colors.green.y * 0.5f, colors.green.z * 0.5f, 1.0f));
        if (ImGui::Button("Apply")) {
            tableData[selectedRow][selectedCol] = std::string(valuePanelBuffer);
            if (selectedRow < static_cast<int>(editedCells.size()) &&
                selectedCol < static_cast<int>(editedCells[selectedRow].size())) {
                editedCells[selectedRow][selectedCol] = true;
            }
            hasChanges = true;
            valuePanelBufferDirty = false;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.red.x * 0.3f, colors.red.y * 0.3f,
                                                      colors.red.z * 0.3f, 1.0f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(colors.red.x * 0.5f, colors.red.y * 0.5f, colors.red.z * 0.5f, 1.0f));
        if (ImGui::Button("Revert")) {
            // Restore from current cell value
            const std::string& currentValue = tableData[selectedRow][selectedCol];
            std::strncpy(valuePanelBuffer, currentValue.c_str(), sizeof(valuePanelBuffer) - 1);
            valuePanelBuffer[sizeof(valuePanelBuffer) - 1] = '\0';
            valuePanelBufferDirty = false;
        }
        ImGui::PopStyleColor(2);
    }
}

void TableViewerTab::renderMetadataTab() {
    const auto& colors = Application::getInstance().getCurrentColors();

    // Find the table metadata
    const Table* foundTable = nullptr;
    for (const auto& table : node_->getTables()) {
        if (table.name == tableName || table.fullName.ends_with("." + tableName)) {
            foundTable = &table;
            break;
        }
    }
    if (!foundTable) {
        for (const auto& view : node_->getViews()) {
            if (view.name == tableName || view.fullName.ends_with("." + tableName)) {
                foundTable = &view;
                break;
            }
        }
    }

    if (!foundTable) {
        ImGui::TextColored(colors.subtext0, "No metadata available.");
        return;
    }

    // Filter input
    ImGui::SetNextItemWidth(-1);
    char filterBuf[128];
    std::strncpy(filterBuf, metadataFilter.c_str(), sizeof(filterBuf) - 1);
    filterBuf[sizeof(filterBuf) - 1] = '\0';
    if (ImGui::InputTextWithHint("##metaFilter", "Filter columns...", filterBuf,
                                 sizeof(filterBuf))) {
        metadataFilter = filterBuf;
    }

    ImGui::Separator();

    // Column metadata table
    if (ImGui::BeginTable("##MetadataColumns", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Null", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("PK", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        // Convert filter to lowercase for case-insensitive matching
        std::string filterLower = metadataFilter;
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

        for (const auto& col : foundTable->columns) {
            // Apply filter
            if (!filterLower.empty()) {
                std::string nameLower = col.name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                if (nameLower.find(filterLower) == std::string::npos) {
                    continue;
                }
            }

            ImGui::TableNextRow();

            // Name column - highlight if it matches selected column
            ImGui::TableSetColumnIndex(0);
            bool isSelectedColumn =
                (selectedCol >= 0 && selectedCol < static_cast<int>(columnNames.size()) &&
                 columnNames[selectedCol] == col.name);
            if (isSelectedColumn) {
                ImGui::TextColored(colors.blue, "%s", col.name.c_str());
            } else {
                ImGui::Text("%s", col.name.c_str());
            }

            // Type column
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(colors.subtext0, "%s", col.type.c_str());

            // Nullable column
            ImGui::TableSetColumnIndex(2);
            if (col.isNotNull) {
                ImGui::TextColored(colors.red, "NOT NULL");
            } else {
                ImGui::TextColored(colors.green, "NULL");
            }

            // PK column
            ImGui::TableSetColumnIndex(3);
            if (col.isPrimaryKey) {
                ImGui::TextColored(colors.yellow, "PK");
            }
        }

        ImGui::EndTable();
    }
}
