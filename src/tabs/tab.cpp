#include "tabs/tab.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "imgui.h"

#include "themes.hpp"
#include <iostream>

// Base Tab class
Tab::Tab(const std::string &name, const TabType type) : name(name), type(type) {}

// SQLEditorTab implementation
SQLEditorTab::SQLEditorTab(const std::string &name) : Tab(name, TabType::SQL_EDITOR) {}

void SQLEditorTab::render() {
    auto &app = Application::getInstance();

    ImGui::Text("SQL Editor");
    ImGui::Separator();

    // SQL input
    ImGui::InputTextMultiline("##SQL", sqlBuffer, sizeof(sqlBuffer),
                              ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.3f));
    sqlQuery = sqlBuffer;

    if (ImGui::Button("Execute Query")) {
        int selectedDb = app.getSelectedDatabase();
        auto &databases = app.getDatabases();

        if (selectedDb >= 0 && selectedDb < (int)databases.size()) {
            auto &db = databases[selectedDb];
            auto [success, error] = db->connect();
            if (success) {
                queryResult = db->executeQuery(sqlQuery);
                strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
                resultBuffer[sizeof(resultBuffer) - 1] = '\0';
            } else {
                queryResult = "Connection failed: " + error;
                strncpy(resultBuffer, queryResult.c_str(), sizeof(resultBuffer) - 1);
                resultBuffer[sizeof(resultBuffer) - 1] = '\0';
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        memset(sqlBuffer, 0, sizeof(sqlBuffer));
        sqlQuery.clear();
    }

    ImGui::Separator();
    ImGui::Text("Results:");

    // Results display
    ImGui::InputTextMultiline("##Results", resultBuffer, sizeof(resultBuffer), ImVec2(-1, -1),
                              ImGuiInputTextFlags_ReadOnly);
}

// TableViewerTab implementation
TableViewerTab::TableViewerTab(const std::string &name, const std::string &databasePath,
                               const std::string &tableName)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(databasePath), tableName(tableName) {
    loadData();
}

void TableViewerTab::render() {
    const auto &colors =
        Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    ImGui::Text("Table: %s", tableName.c_str());
    ImGui::Separator();

    // Pagination controls
    int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;

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

    // Table display
    if (!columnNames.empty() && !tableData.empty()) {
        if (ImGui::BeginTable("TableData", columnNames.size(),
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY)) {
            // Headers
            for (const auto &colName : columnNames) {
                ImGui::TableSetupColumn(colName.c_str());
            }
            ImGui::TableHeadersRow();

            // Data rows
            for (size_t rowIdx = 0; rowIdx < tableData.size(); rowIdx++) {
                const auto &row = tableData[rowIdx];
                ImGui::TableNextRow();

                for (size_t colIdx = 0; colIdx < row.size() && colIdx < columnNames.size();
                     colIdx++) {
                    ImGui::TableNextColumn();

                    // Check if this cell is being edited
                    if (editingRow == (int)rowIdx && editingCol == (int)colIdx) {
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
                        ImGui::PushID((int)(rowIdx * columnNames.size() + colIdx));

                        // Check for cell selection highlighting
                        bool isSelected =
                            (selectedRow == (int)rowIdx && selectedCol == (int)colIdx);
                        if (isSelected) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                                   ImGui::GetColorU32(colors.surface2));
                        }

                        // Use a simple selectable text
                        if (ImGui::Selectable(row[colIdx].c_str(), isSelected,
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            // Single click - select cell
                            selectCell((int)rowIdx, (int)colIdx);

                            // Double click - enter edit mode
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                enterEditMode((int)rowIdx, (int)colIdx);
                            }
                        }

                        ImGui::PopID();
                    }
                }
            }

            ImGui::EndTable();
        }
    } else {
        ImGui::Text("No data to display");
    }
}

void TableViewerTab::loadData() {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();

    // Find database
    std::shared_ptr<DatabaseInterface> db = nullptr;
    for (auto &database : databases) {
        if (database->getPath() == databasePath && database->isConnected()) {
            db = database;
            break;
        }
    }

    if (!db)
        return;

    // Get total row count
    totalRows = db->getRowCount(tableName);

    // Get column names
    columnNames = db->getColumnNames(tableName);

    // Get data with pagination
    int offset = currentPage * rowsPerPage;
    tableData = db->getTableData(tableName, rowsPerPage, offset);

    // Store original data for change tracking
    originalData = tableData;
    hasChanges = false;
}

void TableViewerTab::nextPage() {
    int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    if (currentPage < totalPages - 1) {
        currentPage++;
        loadData();
    }
}

void TableViewerTab::previousPage() {
    if (currentPage > 0) {
        currentPage--;
        loadData();
    }
}

void TableViewerTab::firstPage() {
    currentPage = 0;
    loadData();
}

void TableViewerTab::lastPage() {
    int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    currentPage = totalPages - 1;
    loadData();
}

void TableViewerTab::refreshData() {
    // Reset edit state
    editingRow = -1;
    editingCol = -1;
    selectedRow = -1;
    selectedCol = -1;
    hasChanges = false;

    // Reload data from database
    loadData();
}

void TableViewerTab::saveChanges() {
    // For now, just mark as no changes (would need database update logic)
    hasChanges = false;
    originalData = tableData;

    // TODO: Implement actual database update logic
    // This would require UPDATE SQL statements for each modified cell
}

void TableViewerTab::cancelChanges() {
    // Restore original data
    tableData = originalData;
    hasChanges = false;

    // Reset edit state
    editingRow = -1;
    editingCol = -1;
    selectedRow = -1;
    selectedCol = -1;
}

void TableViewerTab::enterEditMode(int row, int col) {
    if (row >= 0 && row < (int)tableData.size() && col >= 0 && col < (int)columnNames.size()) {

        editingRow = row;
        editingCol = col;

        // Copy current cell value to edit buffer
        strncpy(editBuffer, tableData[row][col].c_str(), sizeof(editBuffer) - 1);
        editBuffer[sizeof(editBuffer) - 1] = '\0';
    }
}

void TableViewerTab::exitEditMode(bool saveEdit) {
    if (editingRow >= 0 && editingCol >= 0) {
        if (saveEdit) {
            // Save the edited value
            std::string newValue = editBuffer;
            if (newValue != tableData[editingRow][editingCol]) {
                tableData[editingRow][editingCol] = newValue;
                hasChanges = true;
            }
        }

        // Clear edit state
        editingRow = -1;
        editingCol = -1;
        memset(editBuffer, 0, sizeof(editBuffer));
    }
}

void TableViewerTab::selectCell(int row, int col) {
    if (row >= 0 && row < (int)tableData.size() && col >= 0 && col < (int)columnNames.size()) {

        selectedRow = row;
        selectedCol = col;
    }
}
