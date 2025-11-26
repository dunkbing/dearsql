#include "ui/tab/table_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "database/query_executor.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <utility>

TableViewerTab::TableViewerTab(const std::string& name, std::string databasePath,
                               const std::string& tableName, PostgresSchemaNode* schemaNode)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)), tableName(tableName),
      databaseNode(schemaNode) {
    initializeTableRenderer();
    initializeFilterAutoComplete();
    loadDataAsync();
}

TableViewerTab::TableViewerTab(const std::string& name, std::string databasePath,
                               std::string tableName, MySQLDatabaseNode* mysqlNode)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)),
      tableName(std::move(tableName)), databaseNode(mysqlNode) {
    initializeTableRenderer();
    initializeFilterAutoComplete();
    loadDataAsync();
}

TableViewerTab::TableViewerTab(const std::string& name, std::string databasePath,
                               std::string tableName, SQLiteDatabase* db)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)),
      tableName(std::move(tableName)), databaseNode(db) {
    initializeTableRenderer();
    initializeFilterAutoComplete();
    loadDataAsync();
}

void TableViewerTab::render() {
    const auto& colors = Application::getInstance().getCurrentColors();

    checkAsyncLoadStatus();

    ImGui::Text("Table: %s", tableName.c_str());
    ImGui::Separator();

    // Filter input with auto-completion
    ImGui::AlignTextToFramePadding(); // Center the label vertically with the input field
    ImGui::Text("Filters:");
    ImGui::SameLine();

    // Use the AutoCompleteInput component
    if (filterAutoComplete &&
        filterAutoComplete->render("##filter", filterBuffer, sizeof(filterBuffer))) {
        applyFilter();
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply Filter")) {
        applyFilter();
    }
    ImGui::SameLine();
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

    // Show current filter if active
    if (!currentFilter.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "Active filter: %s",
                           currentFilter.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(%d rows)", totalRows);
    }

    ImGui::Separator();

    // Pagination controls
    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;

    if (ImGui::Button("<<") && currentPage > 0) {
        firstPage();
    }
    ImGui::SameLine();

    if (ImGui::Button("<") && currentPage > 0) {
        previousPage();
    }
    ImGui::SameLine();

    ImGui::Text("Page %d of %d (%d rows total)", currentPage + 1, totalPages, totalRows);
    ImGui::SameLine();

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

    // Action buttons next to pagination
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
            ImGui::SetTooltip("Cancel");
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_XMARK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Cancel");
        }
    }

    if (hasChanges) {
        ImGui::SameLine();
        ImGui::TextColored(colors.peach, "Unsaved changes");
    }

    ImGui::Separator();

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

        ImGui::Separator();
    }

    // Table display
    if (isLoadingData) {
        ImGui::Text("Loading table data...");
    } else if (!columnNames.empty() && !tableData.empty()) {
        // Update table renderer with current data
        tableRenderer->setColumns(columnNames);
        tableRenderer->setData(tableData);
        tableRenderer->setCellEditedStatus(editedCells);
        tableRenderer->setSelectedCell(selectedRow, selectedCol);
        tableRenderer->setRowNumberOffset(currentPage * rowsPerPage);

        tableRenderer->render("TableData");

        // Handle keyboard navigation - check if we have a selection and the tab is active
        if (selectedRow >= 0 && selectedCol >= 0) {
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
    // Restore original data
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

    // Launch async loading
    dataLoadFuture = std::async(std::launch::async, [this]() {
        try {
            totalRows = databaseNode->getRowCount(tableName, currentFilter);
            columnNames = databaseNode->getColumnNames(tableName);
            const int offset = currentPage * rowsPerPage;
            tableData = databaseNode->getTableData(tableName, rowsPerPage, offset, currentFilter);

            // Store original data for change tracking
            originalData = tableData;
            hasChanges = false;

            // Initialize edited cells tracking
            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(columnNames.size(), false));
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
    }
}

std::vector<std::string> TableViewerTab::getPrimaryKeyColumns() const {
    // Find table columns in node (check both tables and views)
    for (const auto& table : databaseNode->getTables()) {
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
    for (const auto& view : databaseNode->getViews()) {
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

    // Process each edited cell
    for (int rowIdx = 0; rowIdx < editedCells.size(); rowIdx++) {
        for (int colIdx = 0; colIdx < editedCells[rowIdx].size(); colIdx++) {
            if (!editedCells[rowIdx][colIdx]) {
                continue; // Cell not edited
            }

            const std::string& columnName = columnNames[colIdx];
            const std::string& newValue = tableData[rowIdx][colIdx];

            // Build UPDATE statement
            // For PostgreSQL, tableName may be schema-qualified (schema.table)
            std::string tableRef;
            if (const auto dotPos = tableName.find('.'); dotPos != std::string::npos) {
                // schema.table format - quote both parts
                const std::string schemaName = tableName.substr(0, dotPos);
                const std::string tableNameOnly = tableName.substr(dotPos + 1);
                tableRef = std::format(R"("{}"."{}")", schemaName, tableNameOnly);
            } else {
                // simple table name
                tableRef = std::format(R"("{}")", tableName);
            }
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

                // Copy SQL statements and cast database node to IQueryExecutor for async execution
                auto sqlStatements = pendingUpdateSQL;
                auto* executor = dynamic_cast<IQueryExecutor*>(databaseNode);

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
                            const auto result = executor->executeQueryWithResult(sql);
                            std::cout << "SQL Result: "
                                      << (result.success ? result.message : result.errorMessage)
                                      << std::endl;

                            if (!result.success) {
                                allSuccess = false;
                                errorMessage = "Error: " + result.errorMessage;
                                std::cerr << "SQL execution failed: " << result.errorMessage
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
