#include "ui/tab.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "imgui.h"

#include "themes.hpp"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <utility>

// Base Tab class
Tab::Tab(std::string name, const TabType type) : name(std::move(name)), type(type) {}

// SQLEditorTab implementation
SQLEditorTab::SQLEditorTab(const std::string &name, const std::string &databaseConnectionString)
    : Tab(name, TabType::SQL_EDITOR), databaseConnectionString(databaseConnectionString) {
    sqlEditor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);
}

SQLEditorTab::~SQLEditorTab() {
    // Wait for any ongoing query execution to complete
    if (isExecutingQuery && queryExecutionFuture.valid()) {
        queryExecutionFuture.wait();
    }
}

void SQLEditorTab::render() {
    auto &app = Application::getInstance();

    // Check async query execution status
    checkQueryExecutionStatus();

    // Show database connection info if available
    if (!databaseConnectionString.empty()) {
        // Find the database by connection string
        std::shared_ptr<DatabaseInterface> connectedDb = nullptr;
        for (const auto &db : app.getDatabases()) {
            if (db->getConnectionString() == databaseConnectionString ||
                db->getPath() == databaseConnectionString) {
                connectedDb = db;
                break;
            }
        }

        if (connectedDb) {
            ImGui::Text("Connected to: %s", connectedDb->getName().c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Database connection not found");
        }
    } else {
        ImGui::Text("SQL Editor (No specific database)");
    }
    ImGui::Separator();

    // Calculate heights for splitter layout - store total height for splitter reference
    totalContentHeight = ImGui::GetContentRegionAvail().y;
    const float editorHeight = totalContentHeight * splitterPosition;
    const float resultsHeight =
        totalContentHeight * (1.0f - splitterPosition) - 6.0f; // 6px hover area for splitter

    // SQL Editor section
    if (ImGui::BeginChild("SQLEditor", ImVec2(-1, editorHeight), true)) {
        sqlEditor.Render("##SQL", true, ImVec2(-1, -1), true);
        sqlQuery = sqlEditor.GetText();
    }
    ImGui::EndChild();

    // Render splitter
    renderVerticalSplitter("##sql_splitter", &splitterPosition, 100.0f, 200.0f);

    // Results section
    if (ImGui::BeginChild("SQLResults", ImVec2(-1, resultsHeight), true)) {
        // Buttons row
        if (isExecutingQuery) {
            ImGui::BeginDisabled();
            ImGui::Button("Executing...");
            ImGui::EndDisabled();

            ImGui::SameLine();
            UIUtils::Spinner("##query_spinner", 8.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));

            ImGui::SameLine();
            ImGui::Text("Running query...");

            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                cancelQueryExecution();
            }
        } else {
            if (ImGui::Button("Execute Query")) {
                std::shared_ptr<DatabaseInterface> targetDb = nullptr;

                if (!databaseConnectionString.empty()) {
                    // Use the specific database connection
                    for (const auto &db : app.getDatabases()) {
                        if (db->getConnectionString() == databaseConnectionString ||
                            db->getPath() == databaseConnectionString) {
                            targetDb = db;
                            break;
                        }
                    }
                } else {
                    // Fall back to selected database if no specific connection
                    const int selectedDb = app.getSelectedDatabase();
                    const auto &databases = app.getDatabases();
                    if (selectedDb >= 0 && selectedDb < static_cast<int>(databases.size())) {
                        targetDb = databases[selectedDb];
                    }
                }

                if (targetDb) {
                    startQueryExecutionAsync(targetDb, sqlQuery);
                } else {
                    queryResult = "Error: No database connection available";
                    queryError = queryResult;
                    hasStructuredResults = false;
                    queryColumnNames.clear();
                    queryTableData.clear();
                    strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
                    resultBuffer[sizeof(resultBuffer) - 1] = '\0';
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            sqlEditor.SetText("");
            sqlQuery.clear();
            hasStructuredResults = false;
            queryColumnNames.clear();
            queryTableData.clear();
            queryError.clear();
        }

        ImGui::Separator();

        // Results display
        if (!queryError.empty()) {
            // Show error message
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", queryError.c_str());
        } else if (hasStructuredResults && !queryColumnNames.empty()) {
            // Show results in table format using TableRenderer
            if (queryTableData.empty()) {
                // Show column headers even if no data
                ImGui::Text("No rows returned.");
                if (lastQueryDuration.count() > 0) {
                    ImGui::SameLine();
                    ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
                }
            } else {
                // Show row count and execution time on the same line at the top
                ImGui::Text("Rows: %zu", queryTableData.size());
                if (queryTableData.size() >= 1000) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(limited to 1000 rows)");
                }
                if (lastQueryDuration.count() > 0) {
                    ImGui::SameLine();
                    ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
                }

                // Calculate available height for the table within the results child window
                float tableAvailableHeight =
                    ImGui::GetContentRegionAvail().y -
                    20.0f; // Reserve 20px for padding since row count is now above
                tableAvailableHeight =
                    std::max(tableAvailableHeight, 50.0f); // Ensure minimum height of 50px

                // Configure table renderer for query results
                TableRenderer::Config config;
                config.allowEditing = false;
                config.allowSelection = true;
                config.showRowNumbers = false;
                config.minHeight = tableAvailableHeight;

                TableRenderer tableRenderer(config);
                tableRenderer.setColumns(queryColumnNames);
                tableRenderer.setData(queryTableData);

                tableRenderer.render("QueryResults");
            }
        } else if (hasStructuredResults && queryColumnNames.empty()) {
            // Query executed successfully but no results (e.g., INSERT, UPDATE, DELETE)
            ImGui::Text("Query executed successfully.");
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
            }
        } else {
            // Fallback to text display or show empty state
            ImGui::Text("No results to display. Execute a query to see results here.");
        }
    }
    ImGui::EndChild(); // End SQLResults child window
}

void SQLEditorTab::startQueryExecutionAsync(const std::shared_ptr<DatabaseInterface> &targetDb,
                                            const std::string &query) {
    if (isExecutingQuery) {
        return; // Already executing
    }

    isExecutingQuery = true;
    shouldCancelQuery = false;
    hasStructuredResults = false;
    queryError.clear();
    queryColumnNames.clear();
    queryTableData.clear();
    lastQueryDuration = std::chrono::milliseconds{0};

    // Start async query execution
    queryExecutionFuture = std::async(std::launch::async, [this, targetDb, query]() {
        try {
            // Check for cancellation
            if (shouldCancelQuery) {
                return;
            }

            // Connect to database
            auto [success, error] = targetDb->connect();
            if (!success) {
                if (!shouldCancelQuery) {
                    queryResult = "Connection failed: " + error;
                    queryError = queryResult;
                    strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
                    resultBuffer[sizeof(resultBuffer) - 1] = '\0';
                }
                return;
            }

            // Check for cancellation before executing query
            if (shouldCancelQuery) {
                return;
            }

            // Time the query execution
            auto startTime = std::chrono::high_resolution_clock::now();

            // For Redis, also get the text result to check for errors
            std::string textResult;
            if (targetDb->getType() == DatabaseType::REDIS) {
                textResult = targetDb->executeQuery(query);
            }

            // Get structured results for table display
            auto [columnNames, tableData] = targetDb->executeQueryStructured(query);

            auto endTime = std::chrono::high_resolution_clock::now();
            lastQueryDuration =
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            // Check for cancellation before setting results
            if (shouldCancelQuery) {
                return;
            }

            // For Redis, check if the text result contains an error
            if (targetDb->getType() == DatabaseType::REDIS && textResult.find("Error:") == 0) {
                queryResult = textResult;
                queryError = textResult;
                hasStructuredResults = false;
                queryColumnNames.clear();
                queryTableData.clear();
                strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
                resultBuffer[sizeof(resultBuffer) - 1] = '\0';
                return;
            }

            queryColumnNames = columnNames;
            queryTableData = tableData;
            hasStructuredResults = true;

            // Clear any previous error
            queryError.clear();
            queryResult.clear();
            memset(resultBuffer, 0, sizeof(resultBuffer));

        } catch (const std::exception &e) {
            queryResult = "Error executing query: " + std::string(e.what());
            queryError = queryResult;
            hasStructuredResults = false;
            queryColumnNames.clear();
            queryTableData.clear();
            strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
            resultBuffer[sizeof(resultBuffer) - 1] = '\0';
        }
    });
}

void SQLEditorTab::checkQueryExecutionStatus() {
    if (!isExecutingQuery) {
        return;
    }

    if (queryExecutionFuture.valid() &&
        queryExecutionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            queryExecutionFuture.get(); // This will throw if there was an exception
        } catch (const std::exception &e) {
            if (!shouldCancelQuery) {
                queryResult = "Error in async query execution: " + std::string(e.what());
                queryError = queryResult;
                hasStructuredResults = false;
                queryColumnNames.clear();
                queryTableData.clear();
                strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
                resultBuffer[sizeof(resultBuffer) - 1] = '\0';
            }
        }

        isExecutingQuery = false;
    }
}

void SQLEditorTab::cancelQueryExecution() {
    shouldCancelQuery = true;
    // Note: We can't actually cancel the database query once it's started,
    // but we can prevent the results from being processed
    queryResult = "Query execution cancelled by user";
    queryError = queryResult;
    hasStructuredResults = false;
    queryColumnNames.clear();
    queryTableData.clear();
    strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
    resultBuffer[sizeof(resultBuffer) - 1] = '\0';
}

bool SQLEditorTab::renderVerticalSplitter(const char *id, float *position, float minSize1,
                                          float minSize2) const {
    constexpr float hoverThickness = 6.0f;
    constexpr float visualThickness = 2.0f;

    // invisible button with larger hover area
    ImGui::InvisibleButton(id, ImVec2(-1, hoverThickness));

    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    if (hovered || held) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    bool changed = false;
    if (held) {
        const float delta = ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f) {
            // Use the stored total height for consistent calculation
            const float availableHeight = totalContentHeight;

            // Calculate current pixel position
            const float currentPixelPos = *position * availableHeight;
            const float newPixelPos = currentPixelPos + delta;

            // Convert back to normalized position
            float newPosition = newPixelPos / availableHeight;

            // Apply constraints
            const float minPos1 = minSize1 / availableHeight;
            const float maxPos1 = 1.0f - (minSize2 / availableHeight);

            newPosition = std::max(minPos1, std::min(maxPos1, newPosition));

            if (newPosition != *position) {
                *position = newPosition;
                changed = true;
            }
        }
    }

    // Draw thin visual splitter line centered in the hover area
    const ImVec2 minPos = ImGui::GetItemRectMin();
    const ImVec2 maxPos = ImGui::GetItemRectMax();

    constexpr float centerOffset = (hoverThickness - visualThickness) * 0.5f;
    const auto visualMin = ImVec2(minPos.x, minPos.y + centerOffset);
    const auto visualMax = ImVec2(maxPos.x, minPos.y + centerOffset + visualThickness);

    const ImU32 col = ImGui::GetColorU32(held      ? ImGuiCol_SeparatorActive
                                         : hovered ? ImGuiCol_SeparatorHovered
                                                   : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddRectFilled(visualMin, visualMax, col);

    return changed;
}

// TableViewerTab implementation
TableViewerTab::TableViewerTab(const std::string &name, std::string databasePath,
                               std::string tableName)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)),
      tableName(std::move(tableName)) {

    // Initialize table renderer with editable configuration
    TableRenderer::Config config;
    config.allowEditing = true;
    config.allowSelection = true;
    config.showRowNumbers = false;
    config.minHeight = 200.0f;

    tableRenderer = std::make_unique<TableRenderer>(config);

    // Set up callbacks
    tableRenderer->setOnCellEdit([this](int row, int col, const std::string &newValue) {
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

    loadDataAsync();
}

void TableViewerTab::render() {
    const auto &colors =
        Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    checkAsyncLoadStatus();

    ImGui::Text("Table: %s", tableName.c_str());
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

    // Action buttons next to pagination
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20, 0)); // Add some spacing
    ImGui::SameLine();

    if (ImGui::Button("Refresh")) {
        refreshData();
    }

    // Show loading indicator
    if (isLoadingData) {
        ImGui::SameLine();
        ImGui::Text("Loading...");
    }
    ImGui::SameLine();

    if (hasChanges) {
        if (ImGui::Button("Save")) {
            saveChanges();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Save");
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (hasChanges) {
        if (ImGui::Button("Cancel")) {
            cancelChanges();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Cancel");
        ImGui::EndDisabled();
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

        tableRenderer->render("TableData");
    } else {
        ImGui::Text("No data to display");
    }

    // Check async SQL execution status
    checkSQLExecutionStatus();

    // Show save confirmation dialog if needed
    showSaveConfirmationDialog();
}

void TableViewerTab::loadData() {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();

    // Find database by connection string or path
    std::shared_ptr<DatabaseInterface> db = nullptr;
    for (auto &database : databases) {
        if ((database->getConnectionString() == databasePath ||
             database->getPath() == databasePath) &&
            database->isConnected()) {
            db = database;
            break;
        }
    }

    if (!db) {
        return;
    }

    totalRows = db->getRowCount(tableName);
    columnNames = db->getColumnNames(tableName);

    // Get data with pagination
    const int offset = currentPage * rowsPerPage;
    tableData = db->getTableData(tableName, rowsPerPage, offset);

    // Store original data for change tracking
    originalData = tableData;
    hasChanges = false;

    // Initialize edited cells tracking
    editedCells = std::vector<std::vector<bool>>(tableData.size(),
                                                 std::vector<bool>(columnNames.size(), false));
}

void TableViewerTab::nextPage() {
    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    if (currentPage < totalPages - 1 && !isLoadingData) {
        currentPage++;
        loadDataAsync();
    }
}

void TableViewerTab::previousPage() {
    if (currentPage > 0 && !isLoadingData) {
        currentPage--;
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
        for (auto &row : editedCells) {
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
        for (auto &row : editedCells) {
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
    for (auto &row : editedCells) {
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

void TableViewerTab::loadDataAsync() {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();

    // Find database by connection string or path
    std::shared_ptr<DatabaseInterface> db = nullptr;
    for (auto &database : databases) {
        if ((database->getConnectionString() == databasePath ||
             database->getPath() == databasePath) &&
            database->isConnected()) {
            db = database;
            break;
        }
    }

    if (!db) {
        hasLoadingError = true;
        loadingError = "Database not found or not connected";
        return;
    }

    // Clear any previous async result
    if (db->getType() == DatabaseType::SQLITE) {
        db->clearTableDataResult();
    } else {
        db->clearTableDataResult(tableName);
    }

    // Start async data loading (includes metadata)
    const int offset = currentPage * rowsPerPage;
    isLoadingData = true;
    hasLoadingError = false;
    loadingError.clear();

    db->startTableDataLoadAsync(tableName, rowsPerPage, offset);
}

void TableViewerTab::checkAsyncLoadStatus() {
    if (!isLoadingData) {
        return;
    }

    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();

    // Find database by connection string or path
    std::shared_ptr<DatabaseInterface> db = nullptr;
    for (auto &database : databases) {
        if ((database->getConnectionString() == databasePath ||
             database->getPath() == databasePath) &&
            database->isConnected()) {
            db = database;
            break;
        }
    }

    if (!db) {
        isLoadingData = false;
        hasLoadingError = true;
        loadingError = "Database not found or not connected";
        return;
    }

    // Always check the async status first
    if (db->getType() == DatabaseType::SQLITE) {
        db->checkTableDataStatusAsync();

        if (db->hasTableDataResult()) {
            // Load completed - get all data including metadata
            tableData = db->getTableDataResult();
            columnNames = db->getColumnNamesResult();
            totalRows = db->getRowCountResult();
            originalData = tableData;
            hasChanges = false;
            isLoadingData = false;

            // Initialize edited cells tracking
            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(columnNames.size(), false));

            // Clear the result to free memory
            db->clearTableDataResult();
        } else if (!db->isLoadingTableData()) {
            // Loading stopped but no result - probably an error
            isLoadingData = false;
            hasLoadingError = true;
            loadingError = "Failed to load table data";
        }
    } else {
        db->checkTableDataStatusAsync(tableName);

        if (db->hasTableDataResult(tableName)) {
            // Load completed - get all data including metadata
            tableData = db->getTableDataResult(tableName);
            columnNames = db->getColumnNamesResult(tableName);
            totalRows = db->getRowCountResult(tableName);
            originalData = tableData;
            hasChanges = false;
            isLoadingData = false;

            // Initialize edited cells tracking
            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(columnNames.size(), false));

            // Clear the result to free memory
            db->clearTableDataResult(tableName);
        } else if (!db->isLoadingTableData(tableName)) {
            // Loading stopped but no result - probably an error
            isLoadingData = false;
            hasLoadingError = true;
            loadingError = "Failed to load table data";
        }
    }
}

std::vector<std::string> TableViewerTab::getPrimaryKeyColumns() const {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();

    // Find database by connection string or path
    std::shared_ptr<DatabaseInterface> db = nullptr;
    for (auto &database : databases) {
        if ((database->getConnectionString() == databasePath ||
             database->getPath() == databasePath) &&
            database->isConnected()) {
            db = database;
            break;
        }
    }

    std::vector<std::string> pkColumns;
    if (!db) {
        return pkColumns;
    }

    // Find table columns
    const auto &tables = db->getTables();
    for (const auto &table : tables) {
        if (table.name == tableName) {
            for (const auto &column : table.columns) {
                if (column.isPrimaryKey) {
                    pkColumns.push_back(column.name);
                }
            }
            break;
        }
    }

    return pkColumns;
}

std::vector<std::string> TableViewerTab::generateUpdateSQL() {
    std::vector<std::string> sqlStatements;
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();

    // Find database by connection string or path
    std::shared_ptr<DatabaseInterface> db = nullptr;
    for (auto &database : databases) {
        if ((database->getConnectionString() == databasePath ||
             database->getPath() == databasePath) &&
            database->isConnected()) {
            db = database;
            break;
        }
    }

    if (!db) {
        return sqlStatements;
    }

    const bool isSQLite = (db->getType() == DatabaseType::SQLITE);
    const std::vector<std::string> pkColumns = getPrimaryKeyColumns();

    std::cout << "Generating UPDATE SQL for table: " << tableName << std::endl;
    std::cout << "Database type: " << (isSQLite ? "SQLite" : "Postgres") << std::endl;
    std::cout << "Primary key columns: ";
    for (const auto &pk : pkColumns) {
        std::cout << pk << " ";
    }
    std::cout << std::endl;

    // Process each edited cell
    for (int rowIdx = 0; rowIdx < editedCells.size(); rowIdx++) {
        for (int colIdx = 0; colIdx < editedCells[rowIdx].size(); colIdx++) {
            if (!editedCells[rowIdx][colIdx]) {
                continue; // Cell not edited
            }

            const std::string &columnName = columnNames[colIdx];
            const std::string &newValue = tableData[rowIdx][colIdx];

            // Build UPDATE statement
            std::string sql;
            if (isSQLite) {
                sql = std::format("UPDATE {} SET {} = ", tableName, columnName);
            } else {
                sql = std::format(R"(UPDATE "{}" SET "{}" = )", tableName, columnName);
            }

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
                for (const auto &pkCol : pkColumns) {
                    auto pkColIt = std::ranges::find(columnNames, pkCol);
                    if (pkColIt != columnNames.end()) {
                        const int pkColIdx =
                            static_cast<int>(std::distance(columnNames.begin(), pkColIt));
                        const std::string &pkValue = originalData[rowIdx][pkColIdx];

                        if (isSQLite) {
                            whereConditions.push_back(std::format("{} = '{}'", pkCol, pkValue));
                        } else {
                            whereConditions.push_back(std::format("\"{}\" = '{}'", pkCol, pkValue));
                        }
                    }
                }
            } else if (isSQLite) {
                // For SQLite without primary key, use all columns as condition to identify the row
                for (int condColIdx = 0; condColIdx < columnNames.size(); condColIdx++) {
                    const std::string &condValue = originalData[rowIdx][condColIdx];
                    if (condValue == "NULL") {
                        whereConditions.push_back(
                            std::format("{} IS NULL", columnNames[condColIdx]));
                    } else {
                        whereConditions.push_back(
                            std::format("{} = '{}'", columnNames[condColIdx], condValue));
                    }
                }
            } else {
                // For Postgres without primary key, use all columns as condition
                for (int condColIdx = 0; condColIdx < columnNames.size(); condColIdx++) {
                    const std::string &condValue = originalData[rowIdx][condColIdx];
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
            const auto &colors =
                Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
            UIUtils::Spinner("##spinner", 8.0f, 2, ImGui::GetColorU32(colors.blue));

            ImGui::SameLine();
            ImGui::Text("Executing...");
        } else {
            if (ImGui::Button("Execute", ImVec2(120, 0))) {
                // Start async SQL execution
                auto &app = Application::getInstance();
                const auto &databases = app.getDatabases();

                std::shared_ptr<DatabaseInterface> db = nullptr;
                for (auto &database : databases) {
                    if (database->getPath() == databasePath && database->isConnected()) {
                        db = database;
                        break;
                    }
                }

                if (db) {
                    executingSQL = true;

                    // Copy SQL statements and database pointer for async execution
                    auto sqlStatements = pendingUpdateSQL;

                    sqlExecutionFuture = std::async(
                        std::launch::async, [db, sqlStatements]() -> std::pair<bool, std::string> {
                            bool allSuccess = true;
                            std::string errorMessage;

                            for (const auto &sql : sqlStatements) {
                                std::cout << "Executing SQL: " << sql << std::endl;
                                const std::string result = db->executeQuery(sql);
                                std::cout << "SQL Result: " << result << std::endl;

                                if (result.find("Error:") == 0) {
                                    allSuccess = false;
                                    errorMessage = result;
                                    std::cerr << "SQL execution failed: " << result << std::endl;
                                    break;
                                }
                            }

                            if (allSuccess) {
                                std::cout << "All SQL statements executed successfully"
                                          << std::endl;
                            }

                            return {allSuccess, errorMessage};
                        });
                } else {
                    std::cerr << "Database not found or not connected" << std::endl;
                }
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
                for (auto &row : editedCells) {
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

        } catch (const std::exception &e) {
            std::cerr << "Exception during SQL execution: " << e.what() << std::endl;
            executingSQL = false;
        }
    }
}
