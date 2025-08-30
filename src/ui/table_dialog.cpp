#include "ui/table_dialog.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/log_panel.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>

void TableDialog::showTableDialog(const std::shared_ptr<DatabaseInterface>& db,
                                  const std::string& tableName, const std::string& schemaName) {
    database = db;
    targetTableName = tableName;
    targetSchemaName = schemaName;
    dialogMode = TableDialogMode::Edit;

    loadTableStructure();
    resetColumnForm();
    columnEditMode = ColumnEditMode::None;
    selectedColumnIndex = -1;
    rightPanelMode = RightPanelMode::TableProperties;
    isOpen = true;
    hasCompletedResult = false;
    errorMessage.clear();
    previewSQL.clear();
    showPreview = false;

    // Load existing table data if available
    memset(editTableComment, 0, sizeof(editTableComment));
    strncpy(editTableName, targetTableName.c_str(), sizeof(editTableName) - 1);
    editTableName[sizeof(editTableName) - 1] = '\0';
    // TODO: Load actual table comment from database metadata
}

void TableDialog::showCreateTableDialog(const std::shared_ptr<DatabaseInterface>& db,
                                        const std::string& schemaName) {
    database = db;
    targetSchemaName = schemaName;
    dialogMode = TableDialogMode::Create;

    // Initialize with empty table
    targetTableName = "";
    tableColumns.clear();
    memset(newTableName, 0, sizeof(newTableName));
    memset(newTableComment, 0, sizeof(newTableComment));

    resetColumnForm();
    columnEditMode = ColumnEditMode::None;
    selectedColumnIndex = -1;
    rightPanelMode = RightPanelMode::TableProperties;
    isOpen = true;
    hasCompletedResult = false;
    errorMessage.clear();
    previewSQL.clear();
    showPreview = false;
}

void TableDialog::renderDialog() {
    if (!isOpen)
        return;

    const char* title = (dialogMode == TableDialogMode::Create) ? "Create New Table" : "Edit Table";

    // Always try to open the popup when dialog is active
    if (!ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
    }

    // Set popup size - make it larger for the new layout
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal(title, &isOpen, ImGuiWindowFlags_NoScrollbar)) {
        // Show table context
        if (dialogMode == TableDialogMode::Edit) {
            ImGui::Text("Table: %s", targetTableName.c_str());
            if (!targetSchemaName.empty()) {
                ImGui::SameLine();
                ImGui::Text("Schema: %s", targetSchemaName.c_str());
            }
        } else {
            ImGui::Text("Creating new table");
            if (!targetSchemaName.empty()) {
                ImGui::SameLine();
                ImGui::Text("Schema: %s", targetSchemaName.c_str());
            }
        }
        ImGui::Separator();
        ImGui::Spacing();

        // Main content area with splitter
        constexpr float leftPanelWidth = 300.0f;
        const float rightPanelWidth = ImGui::GetContentRegionAvail().x - leftPanelWidth - 10.0f;

        // Left panel - Table structure tree
        ImGui::BeginChild("LeftPanel", ImVec2(leftPanelWidth, -50), true);
        renderLeftPanel();
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel - Column editor
        ImGui::BeginChild("RightPanel", ImVec2(rightPanelWidth, -50), true);
        renderRightPanel();
        ImGui::EndChild();

        // Preview panel (optional)
        if (showPreview) {
            ImGui::Separator();
            renderPreviewPanel();
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Show error message if any
        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("Error: %s", errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        renderButtons();

        ImGui::EndPopup();
    }

    if (!isOpen) {
        // Dialog was closed, reset state
        resetColumnForm();
        errorMessage.clear();
        columnEditMode = ColumnEditMode::None;
        selectedColumnIndex = -1;
        rightPanelMode = RightPanelMode::Instructions;
    }
}

void TableDialog::renderLeftPanel() {
    ImGui::Text("Table Structure");
    ImGui::Separator();
    ImGui::Spacing();

    renderTableTree();
}

void TableDialog::renderRightPanel() {
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
}

void TableDialog::renderTableTree() {
    // Table root node
    ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_DefaultOpen |
                                    ImGuiTreeNodeFlags_OpenOnArrow |
                                    ImGuiTreeNodeFlags_FramePadding;

    // Add selection flag if table properties are being shown
    if (rightPanelMode == RightPanelMode::TableProperties &&
        columnEditMode == ColumnEditMode::None) {
        tableFlags |= ImGuiTreeNodeFlags_Selected;
    }

    std::string displayName;
    if (dialogMode == TableDialogMode::Create) {
        displayName = (strlen(newTableName) > 0 ? std::string(newTableName) : "New Table");
    } else {
        displayName = (strlen(editTableName) > 0 ? std::string(editTableName) : targetTableName);
    }
    const std::string tableLabel = std::format("   {}", displayName);
    bool tableOpen = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

    // Handle table node click
    if (ImGui::IsItemClicked()) {
        rightPanelMode = RightPanelMode::TableProperties;
        columnEditMode = ColumnEditMode::None;
        selectedColumnIndex = -1;
        errorMessage.clear();
    }

    // Draw table icon
    const auto tableIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(
        tableIconPos, ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f)), ICON_FA_TABLE);

    if (tableOpen) {
        renderColumnsNode();
        renderKeysNode();
        ImGui::TreePop();
    }
}

void TableDialog::renderColumnsNode() {
    ImGuiTreeNodeFlags columnsFlags = ImGuiTreeNodeFlags_DefaultOpen |
                                      ImGuiTreeNodeFlags_OpenOnArrow |
                                      ImGuiTreeNodeFlags_FramePadding;

    const std::string columnsLabel =
        std::format("   Columns ({})      ", tableColumns.size()); // Extra spaces for plus icon
    bool columnsOpen = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);

    // Draw columns icon
    const auto columnsIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(
        columnsIconPos, ImGui::GetColorU32(ImVec4(0.5f, 0.9f, 0.5f, 1.0f)), ICON_FA_TABLE_COLUMNS);

    // Draw plus icon for adding columns
    const auto plusIconPos =
        ImVec2(ImGui::GetItemRectMax().x - 25.0f, // Position near the right edge
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);

    // Check if mouse is hovering over the plus icon area
    const ImVec2 plusIconMin = ImVec2(plusIconPos.x - 5.0f, plusIconPos.y - 5.0f);
    const ImVec2 plusIconMax = ImVec2(plusIconPos.x + 15.0f, plusIconPos.y + 15.0f);
    const bool isPlusHovered = ImGui::IsMouseHoveringRect(plusIconMin, plusIconMax);

    // Draw plus icon with hover effect
    ImU32 plusColor =
        isPlusHovered ? ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f))
                      :                                         // Brighter green when hovered
            ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.6f, 0.8f)); // Gray when not hovered

    ImGui::GetWindowDrawList()->AddText(plusIconPos, plusColor, ICON_FA_PLUS);

    // Handle plus icon click
    if (isPlusHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        startAddColumn();
        rightPanelMode = RightPanelMode::ColumnEditor;
    }

    // Show tooltip when hovering over plus icon
    if (isPlusHovered) {
        ImGui::SetTooltip("Add New Column");
    }

    // Context menu for Columns node
    if (ImGui::BeginPopupContextItem("columns_context_menu")) {
        if (ImGui::MenuItem("Add New Column")) {
            startAddColumn();
            rightPanelMode = RightPanelMode::ColumnEditor;
        }
        ImGui::EndPopup();
    }

    if (columnsOpen) {
        for (int i = 0; i < tableColumns.size(); i++) {
            const auto& column = tableColumns[i];

            ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

            if (selectedColumnIndex == i) {
                columnFlags |= ImGuiTreeNodeFlags_Selected;
            }

            // Build column display string
            std::string columnDisplay = std::format("{} ({})", column.name, column.type);
            if (column.isPrimaryKey) {
                columnDisplay += ", PK";
            }
            if (column.isNotNull) {
                columnDisplay += ", NOT NULL";
            }

            ImGui::PushID(i);
            ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);

            if (ImGui::IsItemClicked()) {
                startEditColumn(i);
                rightPanelMode = RightPanelMode::ColumnEditor;
            }

            // Context menu for individual column
            if (ImGui::BeginPopupContextItem("column_context_menu")) {
                if (ImGui::MenuItem("Edit Column")) {
                    startEditColumn(i);
                    rightPanelMode = RightPanelMode::ColumnEditor;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Column")) {
                    // Remove from local list
                    tableColumns.erase(tableColumns.begin() + i);
                    if (selectedColumnIndex == i) {
                        // If we're deleting the currently selected column, go back to table
                        // properties
                        rightPanelMode = RightPanelMode::TableProperties;
                        columnEditMode = ColumnEditMode::None;
                        selectedColumnIndex = -1;
                        resetColumnForm();
                    } else if (selectedColumnIndex > i) {
                        selectedColumnIndex--;
                    }
                    if (showPreview) {
                        updatePreviewSQL();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void TableDialog::renderKeysNode() const {
    constexpr ImGuiTreeNodeFlags keysFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string keysLabel = "   Keys";
    bool keysOpen = ImGui::TreeNodeEx(keysLabel.c_str(), keysFlags);

    // Draw keys icon
    const auto keysIconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(
        keysIconPos, ImGui::GetColorU32(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)), ICON_FA_KEY);

    if (keysOpen) {
        // Show primary key if any column is marked as primary key
        bool hasPrimaryKey = false;
        std::string primaryKeyColumns;
        for (const auto& column : tableColumns) {
            if (column.isPrimaryKey) {
                if (hasPrimaryKey) {
                    primaryKeyColumns += ", ";
                }
                primaryKeyColumns += column.name;
                hasPrimaryKey = true;
            }
        }

        if (hasPrimaryKey) {
            ImGuiTreeNodeFlags pkFlags = ImGuiTreeNodeFlags_Leaf |
                                         ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                         ImGuiTreeNodeFlags_FramePadding;
            std::string pkDisplay = "Primary Key (" + primaryKeyColumns + ")";
            ImGui::TreeNodeEx(pkDisplay.c_str(), pkFlags);
        } else {
            ImGui::Text("  No primary key");
        }
        ImGui::TreePop();
    }
}

void TableDialog::renderColumnEditor() {
    const char* editorTitle =
        (columnEditMode == ColumnEditMode::Add) ? "Add New Column" : "Edit Column";
    ImGui::Text("%s", editorTitle);
    ImGui::Separator();
    ImGui::Spacing();

    // Push style for input fields with borders
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

    // Column Name
    ImGui::Text("Column Name:");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##column_name", columnName, sizeof(columnName))) {
        updateCurrentColumn();
    }

    ImGui::Spacing();

    // Data Type with editable dropdown
    ImGui::Text("Data Type:");
    ImGui::SetNextItemWidth(-1);

    // Create an editable combo box
    if (ImGui::BeginCombo("##column_type", columnType, ImGuiComboFlags_None)) {
        // Add input field at the top for filtering/editing
        ImGui::SetNextItemWidth(-1);

        // Set focus to the input field when combo opens
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        if (ImGui::InputText("##type_filter", columnType, sizeof(columnType))) {
            updateCurrentColumn();
        }

        ImGui::Separator();

        const auto commonTypes = getCommonDataTypes();
        const auto currentInput = std::string(columnType);

        // Filter types based on current input (case-insensitive)
        for (const auto& type : commonTypes) {
            std::string lowerType = type;
            std::string lowerInput = currentInput;
            std::ranges::transform(lowerType, lowerType.begin(), ::tolower);
            std::ranges::transform(lowerInput, lowerInput.begin(), ::tolower);

            if (lowerInput.empty() || lowerType.find(lowerInput) != std::string::npos) {
                bool isSelected = (type == currentInput);
                if (ImGui::Selectable(type.c_str(), isSelected)) {
                    strncpy(columnType, type.c_str(), sizeof(columnType) - 1);
                    columnType[sizeof(columnType) - 1] = '\0';
                    updateCurrentColumn();
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Constraints
    ImGui::Text("Constraints:");
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

    ImGui::Spacing();

    // Default Value
    ImGui::Text("Default Value (optional):");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##default_value", defaultValue, sizeof(defaultValue))) {
        updateCurrentColumn();
    }

    ImGui::Spacing();

    // Comment (if supported by database)
    if (database->getType() == DatabaseType::MYSQL ||
        database->getType() == DatabaseType::POSTGRESQL) {
        ImGui::Text("Comment:");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##column_comment", columnComment, sizeof(columnComment),
                                      ImVec2(0, 60))) {
            updateCurrentColumn();
        }
    }

    // Pop style
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // Preview toggle
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Checkbox("Show SQL Preview", &showPreview);
    if (showPreview) {
        updatePreviewSQL();
    }
}

void TableDialog::renderPreviewPanel() const {
    ImGui::Text("SQL Preview:");
    ImGui::Separator();

    if (!previewSQL.empty()) {
        ImGui::TextWrapped("%s", previewSQL.c_str());
    } else {
        ImGui::TextDisabled("No changes to preview");
    }
}

void TableDialog::renderButtons() {
    const auto& colors = Application::getInstance().getCurrentColors();

    // Save button
    ImGui::PushStyleColor(ImGuiCol_Button, colors.blue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.sky);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.sapphire);

    if (ImGui::Button("Save", ImVec2(120, 0))) {
        if (dialogMode == TableDialogMode::Create) {
            if (validateTableInput() && executeCreateTable()) {
                hasCompletedResult = true;
                isOpen = false;
            }
        } else {
            // For edit mode, save any pending changes
            if (saveTableChanges()) {
                hasCompletedResult = true;
                isOpen = false;
            }
        }
    }

    ImGui::PopStyleColor(3);
    ImGui::SameLine();

    // Close button
    ImGui::PushStyleColor(ImGuiCol_Button, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.overlay1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay2);

    if (ImGui::Button("Close", ImVec2(120, 0))) {
        isOpen = false;
    }

    ImGui::PopStyleColor(3);
}

void TableDialog::startAddColumn() {
    // Create a new column with default values
    Column newColumn;
    newColumn.name = "column_name";
    newColumn.type = "VARCHAR(255)";
    newColumn.comment = "";
    newColumn.isPrimaryKey = false;
    newColumn.isNotNull = false;

    tableColumns.push_back(newColumn);

    // Set up the editor for the new column
    columnEditMode = ColumnEditMode::Edit;
    selectedColumnIndex = static_cast<int>(tableColumns.size() - 1);
    rightPanelMode = RightPanelMode::ColumnEditor;

    // Populate the form with the new column data
    populateColumnFormFromColumn(newColumn);
    errorMessage.clear();
    updatePreviewSQL();
}

void TableDialog::startEditColumn(int columnIndex) {
    if (columnIndex >= 0 && columnIndex < tableColumns.size()) {
        columnEditMode = ColumnEditMode::Edit;
        selectedColumnIndex = columnIndex;
        rightPanelMode = RightPanelMode::ColumnEditor;
        originalColumnName = tableColumns[columnIndex].name;
        populateColumnFormFromColumn(tableColumns[columnIndex]);
        errorMessage.clear();
    }
}

void TableDialog::cancelColumnEdit() {
    columnEditMode = ColumnEditMode::None;
    selectedColumnIndex = -1;
    rightPanelMode = RightPanelMode::TableProperties;
    resetColumnForm();
    errorMessage.clear();
    previewSQL.clear();
}

bool TableDialog::saveColumn() {
    // This method is no longer used since we update in real-time
    // Just validate the current input
    return validateColumnInput();
}

bool TableDialog::validateColumnInput() {
    errorMessage.clear();

    // Check column name
    if (strlen(columnName) == 0) {
        errorMessage = "Column name cannot be empty";
        return false;
    }

    // Check data type
    if (strlen(columnType) == 0) {
        errorMessage = "Data type cannot be empty";
        return false;
    }

    return true;
}

bool TableDialog::executeAddColumn() {
    try {
        std::string sql = generateAddColumnSQL();
        LogPanel::info("Executing: " + sql);

        // For PostgreSQL with comments, we need to execute multiple statements
        if (database->getType() == DatabaseType::POSTGRESQL && strlen(columnComment) > 0) {
            // Split the SQL into separate statements
            size_t semicolonPos = sql.find(';');
            if (semicolonPos != std::string::npos) {
                std::string addColumnSQL = sql.substr(0, semicolonPos);
                std::string commentSQL = sql.substr(semicolonPos + 1);

                // Execute ADD COLUMN first
                std::string result1 = database->executeQuery(addColumnSQL);
                if (result1.find("ERROR") != std::string::npos ||
                    result1.find("Error") != std::string::npos) {
                    const std::string& cleanError = result1;
                    if (cleanError.find("already exists") != std::string::npos) {
                        errorMessage = "Column '" + std::string(columnName) +
                                       "' already exists in table '" + targetTableName + "'";
                    } else {
                        errorMessage = "Failed to add column: " + result1;
                    }
                    return false;
                }

                // Execute COMMENT statement
                std::string result2 = database->executeQuery(commentSQL);
                if (result2.find("ERROR") != std::string::npos ||
                    result2.find("Error") != std::string::npos) {
                    // Column was added but comment failed - log warning but don't fail
                    LogPanel::warn("Column added but comment failed: " + result2);
                }
            }
        } else {
            // Single statement execution
            std::string result = database->executeQuery(sql);

            // Check if there was an error in the result
            if (result.find("ERROR") != std::string::npos ||
                result.find("Error") != std::string::npos) {
                const std::string& cleanError = result;
                if (cleanError.find("already exists") != std::string::npos) {
                    errorMessage = "Column '" + std::string(columnName) +
                                   "' already exists in table '" + targetTableName + "'";
                } else {
                    errorMessage = "Failed to add column: " + result;
                }
                return false;
            }
        }

        // Refresh table structure
        database->setTablesLoaded(false);
        database->refreshTables();

        LogPanel::info("Column '" + std::string(columnName) + "' added successfully to table '" +
                       targetTableName + "'");
        return true;

    } catch (const std::exception& e) {
        errorMessage = "Failed to add column: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}

bool TableDialog::executeEditColumn() {
    try {
        std::string sql = generateEditColumnSQL();
        if (sql.empty()) {
            return false; // Error message already set in generateEditColumnSQL
        }

        LogPanel::info("Executing: " + sql);

        // For PostgreSQL, we need to execute multiple statements
        if (database->getType() == DatabaseType::POSTGRESQL) {
            // Split the SQL into separate statements
            std::vector<std::string> statements;
            std::string currentStatement;
            std::istringstream sqlStream(sql);

            while (std::getline(sqlStream, currentStatement, ';')) {
                // Trim whitespace
                currentStatement.erase(0, currentStatement.find_first_not_of(" \t\n\r"));
                currentStatement.erase(currentStatement.find_last_not_of(" \t\n\r") + 1);

                if (!currentStatement.empty()) {
                    statements.push_back(currentStatement);
                }
            }

            // Execute each statement
            for (const auto& statement : statements) {
                std::string result = database->executeQuery(statement);
                if (result.find("ERROR") != std::string::npos ||
                    result.find("Error") != std::string::npos) {
                    errorMessage = "Failed to edit column: " + result;
                    return false;
                }
            }
        } else {
            // Single statement execution
            std::string result = database->executeQuery(sql);

            // Check if there was an error in the result
            if (result.find("ERROR") != std::string::npos ||
                result.find("Error") != std::string::npos) {
                errorMessage = "Failed to edit column: " + result;
                return false;
            }
        }

        // Refresh table structure
        database->setTablesLoaded(false);
        database->refreshTables();

        LogPanel::info("Column '" + originalColumnName + "' updated successfully in table '" +
                       targetTableName + "'");
        return true;

    } catch (const std::exception& e) {
        errorMessage = "Failed to edit column: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}

void TableDialog::resetColumnForm() {
    memset(columnName, 0, sizeof(columnName));
    memset(columnType, 0, sizeof(columnType));
    memset(columnComment, 0, sizeof(columnComment));
    memset(defaultValue, 0, sizeof(defaultValue));

    isPrimaryKey = false;
    isNotNull = false;
    isUnique = false;
}

void TableDialog::populateColumnFormFromColumn(const Column& column) {
    strncpy(columnName, column.name.c_str(), sizeof(columnName) - 1);
    strncpy(columnType, column.type.c_str(), sizeof(columnType) - 1);
    strncpy(columnComment, column.comment.c_str(), sizeof(columnComment) - 1);

    isPrimaryKey = column.isPrimaryKey;
    isNotNull = column.isNotNull;
}

void TableDialog::loadTableStructure() {
    tableColumns.clear();

    // Find the table in the database's table list
    const auto& tables = database->getTables();
    for (const auto& table : tables) {
        if (table.name == targetTableName) {
            tableColumns = table.columns;
            break;
        }
    }
}

void TableDialog::updatePreviewSQL() {
    if (dialogMode == TableDialogMode::Create) {
        previewSQL = generateCreateTableSQL();
    } else if (columnEditMode == ColumnEditMode::Add) {
        previewSQL = generateAddColumnSQL();
    } else if (columnEditMode == ColumnEditMode::Edit) {
        previewSQL = generateEditColumnSQL();
    } else {
        previewSQL.clear();
    }
}

std::string TableDialog::generateAddColumnSQL() {
    // For PostgreSQL, ensure table name is schema-qualified
    std::string qualifiedTableName = targetTableName;
    if (database->getType() == DatabaseType::POSTGRESQL) {
        // If table name doesn't already contain a schema prefix, add schema
        if (qualifiedTableName.find('.') == std::string::npos) {
            const std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
            qualifiedTableName = schemaName + "." + qualifiedTableName;
        }
    }

    std::string sql = "ALTER TABLE " + qualifiedTableName + " ADD COLUMN " +
                      std::string(columnName) + " " + std::string(columnType);

    // Add constraints
    if (isNotNull) {
        sql += " NOT NULL";
    }

    if (isUnique) {
        sql += " UNIQUE";
    }

    if (strlen(defaultValue) > 0) {
        sql += " DEFAULT " + std::string(defaultValue);
    }

    // Handle comments differently for different databases
    if (strlen(columnComment) > 0) {
        if (database->getType() == DatabaseType::MYSQL) {
            // MySQL supports COMMENT in ALTER TABLE ADD COLUMN
            sql += " COMMENT '" + std::string(columnComment) + "'";
        } else if (database->getType() == DatabaseType::POSTGRESQL) {
            // PostgreSQL requires a separate COMMENT ON COLUMN statement
            sql += "; COMMENT ON COLUMN " + qualifiedTableName + "." + std::string(columnName) +
                   " IS '" + std::string(columnComment) + "'";
        }
    }

    return sql;
}

std::string TableDialog::generateEditColumnSQL() {
    std::string sql;

    // Different databases have different syntax for altering columns
    switch (database->getType()) {
    case DatabaseType::POSTGRESQL: {
        std::string qualifiedTableName = targetTableName;
        if (qualifiedTableName.find('.') == std::string::npos) {
            const std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
            qualifiedTableName = schemaName + "." + qualifiedTableName;
        }

        // PostgreSQL uses ALTER COLUMN for each property
        std::vector<std::string> statements;

        // Rename column if needed
        if (std::string(columnName) != originalColumnName) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " RENAME COLUMN " +
                                 originalColumnName + " TO " + std::string(columnName));
        }

        // Change column type
        statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                             std::string(columnName) + " TYPE " + std::string(columnType));

        // Handle NOT NULL constraint
        if (isNotNull) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " SET NOT NULL");
        } else {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " DROP NOT NULL");
        }

        // Handle default value
        if (strlen(defaultValue) > 0) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " SET DEFAULT " +
                                 std::string(defaultValue));
        } else {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " DROP DEFAULT");
        }

        // Handle comment
        if (strlen(columnComment) > 0) {
            statements.push_back("COMMENT ON COLUMN " + qualifiedTableName + "." +
                                 std::string(columnName) + " IS '" + std::string(columnComment) +
                                 "'");
        }

        // Join all statements with semicolons
        for (size_t i = 0; i < statements.size(); ++i) {
            if (i > 0)
                sql += "; ";
            sql += statements[i];
        }
        break;
    }

    case DatabaseType::MYSQL:
        // MySQL uses MODIFY COLUMN
        sql = "ALTER TABLE " + targetTableName + " MODIFY COLUMN " + std::string(columnName) + " " +
              std::string(columnType);

        if (isNotNull) {
            sql += " NOT NULL";
        }

        if (strlen(defaultValue) > 0) {
            sql += " DEFAULT " + std::string(defaultValue);
        }

        if (strlen(columnComment) > 0) {
            sql += " COMMENT '" + std::string(columnComment) + "'";
        }
        break;

    case DatabaseType::SQLITE:
        // SQLite doesn't support ALTER COLUMN directly
        errorMessage =
            "SQLite doesn't support column modification. You need to recreate the table.";
        return "";

    default:
        errorMessage = "Column editing not supported for this database type";
        return "";
    }

    return sql;
}

std::vector<std::string> TableDialog::getCommonDataTypes() const {
    std::vector<std::string> types;

    switch (database->getType()) {
    case DatabaseType::POSTGRESQL:
        types = {
            "INTEGER",      "BIGINT", "SMALLINT", "DECIMAL", "NUMERIC", "REAL", "DOUBLE PRECISION",
            "VARCHAR(255)", "TEXT",   "CHAR(10)", "BOOLEAN", "DATE",    "TIME", "TIMESTAMP",
            "UUID",         "JSON",   "JSONB"};
        break;

    case DatabaseType::MYSQL:
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

void TableDialog::renderTableProperties() {
    ImGui::Text("Table Properties");
    ImGui::Separator();
    ImGui::Spacing();

    // Push style for input fields with borders
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

    if (dialogMode == TableDialogMode::Create) {
        // Table Name
        ImGui::Text("Table Name:");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##table_name", newTableName, sizeof(newTableName))) {
            if (showPreview) {
                updatePreviewSQL();
            }
        }
        ImGui::Spacing();

        // Comment (if supported by database)
        if (database->getType() == DatabaseType::MYSQL ||
            database->getType() == DatabaseType::POSTGRESQL) {
            ImGui::Text("Comment:");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextMultiline("##table_comment", newTableComment,
                                          sizeof(newTableComment), ImVec2(0, 60))) {
                if (showPreview) {
                    updatePreviewSQL();
                }
            }
            ImGui::Spacing();
        }
    } else {
        // Edit mode - allow table name editing
        ImGui::Text("Table Name:");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##edit_table_name", editTableName, sizeof(editTableName))) {
            if (showPreview) {
                updatePreviewSQL();
            }
        }

        if (!targetSchemaName.empty()) {
            ImGui::Text("Schema: %s", targetSchemaName.c_str());
        }
        ImGui::Spacing();

        // Comment (if supported by database)
        if (database->getType() == DatabaseType::MYSQL ||
            database->getType() == DatabaseType::POSTGRESQL) {
            ImGui::Text("Comment:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextMultiline("##edit_table_comment", editTableComment,
                                      sizeof(editTableComment), ImVec2(0, 60));
            ImGui::Spacing();
        }
    }

    // Pop style
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::Spacing();

    // Instructions
    if (dialogMode == TableDialogMode::Create) {
        ImGui::TextWrapped(
            "Add columns to your table by right-clicking on 'Columns' in the tree on the left.");

        if (tableColumns.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "Note: A table must have at least one column to be created.");
        }
    } else {
        ImGui::TextWrapped(
            "Manage your table structure using the tree on the left. Right-click on 'Columns' to "
            "add new columns, or click on existing columns to edit them.");
    }

    // Preview toggle
    ImGui::Spacing();
    ImGui::Checkbox("Show SQL Preview", &showPreview);
    if (showPreview) {
        updatePreviewSQL();
    }
}

void TableDialog::renderInstructions() {
    ImGui::Text("Table Editor");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("Click on the table name in the tree to edit table properties, or select a "
                       "column to edit it. Right-click on 'Columns' to add a new column.");
}

bool TableDialog::validateTableInput() {
    errorMessage.clear();

    // Check table name
    if (strlen(newTableName) == 0) {
        errorMessage = "Table name cannot be empty";
        return false;
    }

    // Check if table has at least one column
    if (tableColumns.empty()) {
        errorMessage = "Table must have at least one column";
        return false;
    }

    // Check if table has a primary key (recommended but not required)
    bool hasPrimaryKey = false;
    for (const auto& column : tableColumns) {
        if (column.isPrimaryKey) {
            hasPrimaryKey = true;
            break;
        }
    }

    if (!hasPrimaryKey) {
        // This is just a warning, not an error
        LogPanel::warn("Table '" + std::string(newTableName) + "' does not have a primary key");
    }

    return true;
}

bool TableDialog::executeCreateTable() {
    try {
        std::string sql = generateCreateTableSQL();
        LogPanel::info("Executing: " + sql);

        std::string result = database->executeQuery(sql);

        // Check if there was an error in the result
        if (result.find("ERROR") != std::string::npos ||
            result.find("Error") != std::string::npos) {
            const std::string& cleanError = result;
            if (cleanError.find("already exists") != std::string::npos) {
                errorMessage = "Table '" + std::string(newTableName) + "' already exists";
            } else {
                errorMessage = "Failed to create table: " + result;
            }
            return false;
        }

        // Add comment if supported and provided
        if ((database->getType() == DatabaseType::MYSQL ||
             database->getType() == DatabaseType::POSTGRESQL) &&
            strlen(newTableComment) > 0) {

            std::string commentSQL;
            if (database->getType() == DatabaseType::POSTGRESQL) {
                auto qualifiedTableName = std::string(newTableName);
                if (qualifiedTableName.find('.') == std::string::npos) {
                    std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
                    qualifiedTableName = schemaName + "." + qualifiedTableName;
                }
                commentSQL = "COMMENT ON TABLE " + qualifiedTableName + " IS '" +
                             std::string(newTableComment) + "'";
            } else if (database->getType() == DatabaseType::MYSQL) {
                commentSQL = "ALTER TABLE " + std::string(newTableName) + " COMMENT = '" +
                             std::string(newTableComment) + "'";
            }

            if (!commentSQL.empty()) {
                std::string commentResult = database->executeQuery(commentSQL);
                if (commentResult.find("ERROR") != std::string::npos ||
                    commentResult.find("Error") != std::string::npos) {
                    // Table was created but comment failed - log warning but don't fail
                    LogPanel::warn("Table created but comment failed: " + commentResult);
                }
            }
        }

        // Refresh table structure
        database->setTablesLoaded(false);
        database->refreshTables();

        LogPanel::info("Table '" + std::string(newTableName) + "' created successfully");
        return true;

    } catch (const std::exception& e) {
        errorMessage = "Failed to create table: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}

std::string TableDialog::generateCreateTableSQL() {
    if (tableColumns.empty()) {
        return "";
    }

    std::string qualifiedTableName = std::string(newTableName);
    if (database->getType() == DatabaseType::POSTGRESQL) {
        // If table name doesn't already contain a schema prefix, add schema
        if (qualifiedTableName.find('.') == std::string::npos) {
            std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
            qualifiedTableName = schemaName + "." + qualifiedTableName;
        }
    }

    std::string sql = "CREATE TABLE " + qualifiedTableName + " (\n";

    // Add columns
    for (size_t i = 0; i < tableColumns.size(); ++i) {
        const auto& column = tableColumns[i];
        sql += "    " + column.name + " " + column.type;

        if (column.isNotNull) {
            sql += " NOT NULL";
        }

        if (!column.comment.empty() && database->getType() == DatabaseType::MYSQL) {
            sql += " COMMENT '" + column.comment + "'";
        }

        if (i < tableColumns.size() - 1) {
            sql += ",";
        }
        sql += "\n";
    }

    // Add primary key constraint if any columns are marked as primary key
    std::vector<std::string> primaryKeyColumns;
    for (const auto& column : tableColumns) {
        if (column.isPrimaryKey) {
            primaryKeyColumns.push_back(column.name);
        }
    }

    if (!primaryKeyColumns.empty()) {
        sql += ",\n    PRIMARY KEY (";
        for (size_t i = 0; i < primaryKeyColumns.size(); ++i) {
            sql += primaryKeyColumns[i];
            if (i < primaryKeyColumns.size() - 1) {
                sql += ", ";
            }
        }
        sql += ")\n";
    }

    sql += ")";

    // Add table comment for MySQL (PostgreSQL uses separate COMMENT ON TABLE statement)
    if (database->getType() == DatabaseType::MYSQL && strlen(newTableComment) > 0) {
        sql += " COMMENT = '" + std::string(newTableComment) + "'";
    }

    return sql;
}

void TableDialog::updateCurrentColumn() {
    if (selectedColumnIndex >= 0 && selectedColumnIndex < tableColumns.size()) {
        auto& column = tableColumns[selectedColumnIndex];
        column.name = std::string(columnName);
        column.type = std::string(columnType);
        column.comment = std::string(columnComment);
        column.isPrimaryKey = isPrimaryKey;
        column.isNotNull = isNotNull;

        // Update preview if it's showing
        if (showPreview) {
            updatePreviewSQL();
        }
    }
}

bool TableDialog::saveTableChanges() {
    try {
        std::vector<std::string> sqlStatements;

        // Check if table name changed
        if (std::string(editTableName) != targetTableName) {
            std::string renameSQL;
            if (database->getType() == DatabaseType::POSTGRESQL) {
                std::string qualifiedOldName = targetTableName;
                auto qualifiedNewName = std::string(editTableName);
                if (qualifiedOldName.find('.') == std::string::npos) {
                    std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
                    qualifiedOldName = schemaName + "." + qualifiedOldName;
                }
                renameSQL =
                    "ALTER TABLE " + qualifiedOldName + " RENAME TO " + std::string(editTableName);
            } else if (database->getType() == DatabaseType::MYSQL) {
                renameSQL =
                    "ALTER TABLE " + targetTableName + " RENAME TO " + std::string(editTableName);
            } else if (database->getType() == DatabaseType::SQLITE) {
                renameSQL =
                    "ALTER TABLE " + targetTableName + " RENAME TO " + std::string(editTableName);
            }

            if (!renameSQL.empty()) {
                sqlStatements.push_back(renameSQL);
            }
        }

        // Get original table structure for comparison
        std::vector<Column> originalColumns;
        const auto& tables = database->getTables();
        for (const auto& table : tables) {
            if (table.name == targetTableName) {
                originalColumns = table.columns;
                break;
            }
        }

        // Compare current columns with original columns and generate ALTER statements
        for (const auto& currentColumn : tableColumns) {
            // Find corresponding original column
            bool foundOriginal = false;
            for (const auto& originalColumn : originalColumns) {
                if (originalColumn.name == currentColumn.name) {
                    foundOriginal = true;
                    // Check if column properties changed
                    if (originalColumn.type != currentColumn.type ||
                        originalColumn.isNotNull != currentColumn.isNotNull ||
                        originalColumn.isPrimaryKey != currentColumn.isPrimaryKey ||
                        originalColumn.comment != currentColumn.comment) {

                        // Generate ALTER COLUMN statement
                        std::string alterSQL =
                            generateEditColumnSQLForColumn(currentColumn, originalColumn.name);
                        if (!alterSQL.empty()) {
                            sqlStatements.push_back(alterSQL);
                        }
                    }
                    break;
                }
            }

            // If not found in original, it's a new column
            if (!foundOriginal) {
                std::string addSQL = generateAddColumnSQLForColumn(currentColumn);
                if (!addSQL.empty()) {
                    sqlStatements.push_back(addSQL);
                }
            }
        }

        // Check for deleted columns (columns that were in original but not in current)
        for (const auto& originalColumn : originalColumns) {
            bool foundInCurrent = false;
            for (const auto& currentColumn : tableColumns) {
                if (currentColumn.name == originalColumn.name) {
                    foundInCurrent = true;
                    break;
                }
            }

            if (!foundInCurrent) {
                // Generate DROP COLUMN statement
                std::string dropSQL;
                std::string tableName = (std::string(editTableName) != targetTableName)
                                            ? std::string(editTableName)
                                            : targetTableName;

                if (database->getType() == DatabaseType::POSTGRESQL) {
                    std::string qualifiedTableName = tableName;
                    if (qualifiedTableName.find('.') == std::string::npos) {
                        std::string schemaName =
                            targetSchemaName.empty() ? "public" : targetSchemaName;
                        qualifiedTableName = std::format("{}.{}", schemaName, qualifiedTableName);
                    }
                    dropSQL =
                        "ALTER TABLE " + qualifiedTableName + " DROP COLUMN " + originalColumn.name;
                } else if (database->getType() == DatabaseType::MYSQL) {
                    dropSQL = "ALTER TABLE " + tableName + " DROP COLUMN " + originalColumn.name;
                } else if (database->getType() == DatabaseType::SQLITE) {
                    // SQLite doesn't support DROP COLUMN directly
                    LogPanel::warn("SQLite doesn't support DROP COLUMN. Column '" +
                                   originalColumn.name + "' will remain in the table.");
                    continue;
                }

                if (!dropSQL.empty()) {
                    sqlStatements.push_back(dropSQL);
                }
            }
        }

        // Execute all SQL statements
        for (const auto& sql : sqlStatements) {
            LogPanel::info("Executing: " + sql);
            std::string result = database->executeQuery(sql);

            if (result.find("ERROR") != std::string::npos ||
                result.find("Error") != std::string::npos) {
                errorMessage = std::format("Failed to execute: {}", sql);
                LogPanel::error(errorMessage);
                return false;
            }
        }

        // Refresh table structure
        database->setTablesLoaded(false);
        database->refreshTables();

        LogPanel::info("Table changes saved successfully");
        return true;

    } catch (const std::exception& e) {
        errorMessage = "Failed to save table changes: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}
std::string TableDialog::generateAddColumnSQLForColumn(const Column& column) {
    std::string tableName = (std::string(editTableName) != targetTableName)
                                ? std::string(editTableName)
                                : targetTableName;

    std::string qualifiedTableName = tableName;
    if (database->getType() == DatabaseType::POSTGRESQL) {
        if (qualifiedTableName.find('.') == std::string::npos) {
            std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
            qualifiedTableName = schemaName + "." + qualifiedTableName;
        }
    }

    std::string sql =
        "ALTER TABLE " + qualifiedTableName + " ADD COLUMN " + column.name + " " + column.type;

    if (column.isNotNull) {
        sql += " NOT NULL";
    }

    // Handle comments differently for different databases
    if (!column.comment.empty()) {
        if (database->getType() == DatabaseType::MYSQL) {
            sql += " COMMENT '" + column.comment + "'";
        } else if (database->getType() == DatabaseType::POSTGRESQL) {
            sql += "; COMMENT ON COLUMN " + qualifiedTableName + "." + column.name + " IS '" +
                   column.comment + "'";
        }
    }

    return sql;
}

std::string TableDialog::generateEditColumnSQLForColumn(const Column& column,
                                                        const std::string& originalName) {
    std::string tableName = (std::string(editTableName) != targetTableName)
                                ? std::string(editTableName)
                                : targetTableName;

    std::string sql;

    switch (database->getType()) {
    case DatabaseType::POSTGRESQL: {
        std::string qualifiedTableName = tableName;
        if (qualifiedTableName.find('.') == std::string::npos) {
            std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
            qualifiedTableName = schemaName + "." + qualifiedTableName;
        }

        std::vector<std::string> statements;

        // Rename column if needed
        if (column.name != originalName) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " RENAME COLUMN " +
                                 originalName + " TO " + column.name);
        }

        // Change column type
        statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " + column.name +
                             " TYPE " + column.type);

        // Handle NOT NULL constraint
        if (column.isNotNull) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 column.name + " SET NOT NULL");
        } else {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 column.name + " DROP NOT NULL");
        }

        // Handle comment
        if (!column.comment.empty()) {
            statements.push_back("COMMENT ON COLUMN " + qualifiedTableName + "." + column.name +
                                 " IS '" + column.comment + "'");
        }

        // Join all statements with semicolons
        for (size_t i = 0; i < statements.size(); ++i) {
            if (i > 0)
                sql += "; ";
            sql += statements[i];
        }
        break;
    }

    case DatabaseType::MYSQL:
        // MySQL uses MODIFY COLUMN
        sql = "ALTER TABLE " + tableName + " MODIFY COLUMN " + column.name + " " + column.type;

        if (column.isNotNull) {
            sql += " NOT NULL";
        }

        if (!column.comment.empty()) {
            sql += " COMMENT '" + column.comment + "'";
        }
        break;

    case DatabaseType::SQLITE:
        // SQLite doesn't support ALTER COLUMN directly
        LogPanel::warn("SQLite doesn't support column modification for column: " + column.name);
        return "";

    default:
        LogPanel::warn("Column editing not supported for this database type");
        return "";
    }

    return sql;
}
