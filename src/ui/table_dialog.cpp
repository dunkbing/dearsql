#include "ui/table_dialog.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/sql_builder.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <format>

TableDialog& TableDialog::instance() {
    static TableDialog inst;
    return inst;
}

void TableDialog::showCreate(IDatabaseNode* node, const std::string& schema) {
    reset();
    dialogMode = TableDialogMode::Create;
    dbNode = node;
    if (node) {
        databaseType = node->getDatabaseType();
    }
    schemaName = schema;
    cancelCallback = nullptr;

    editingTable = Table{};
    editingTable.columns.clear();

    rightPanelMode = RightPanelMode::TableProperties;
    isDialogOpen = true;
}

void TableDialog::showEdit(IDatabaseNode* node, const Table& table, const std::string& schema) {
    reset();
    dialogMode = TableDialogMode::Edit;
    dbNode = node;
    databaseType = node ? node->getDatabaseType() : DatabaseType::SQLITE;
    schemaName = schema;
    cancelCallback = nullptr;

    editingTable = table;
    originalTable = table;

    std::strncpy(tableNameBuffer, table.name.c_str(), sizeof(tableNameBuffer) - 1);
    tableNameBuffer[sizeof(tableNameBuffer) - 1] = '\0';
    std::memset(tableCommentBuffer, 0, sizeof(tableCommentBuffer));

    rightPanelMode = RightPanelMode::TableProperties;
    isDialogOpen = true;
}

void TableDialog::render() {
    if (!isDialogOpen)
        return;

    const char* title = (dialogMode == TableDialogMode::Create) ? "Create New Table" : "Edit Table";

    // Always try to open the popup when dialog is active
    if (!ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
    }

    // Set popup size - make it larger for the new layout
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    // Square corners
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

    if (ImGui::BeginPopupModal(title, &isDialogOpen, ImGuiWindowFlags_NoScrollbar)) {
        // Show table context
        if (dialogMode == TableDialogMode::Edit) {
            ImGui::Text("Table: %s", originalTable.name.c_str());
            if (!schemaName.empty()) {
                ImGui::SameLine();
                ImGui::Text("Schema: %s", schemaName.c_str());
            }
        } else {
            ImGui::Text("Creating new table");
            if (!schemaName.empty()) {
                ImGui::SameLine();
                ImGui::Text("in schema: %s", schemaName.c_str());
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

    ImGui::PopStyleVar(3); // WindowRounding, PopupRounding, FrameRounding

    if (!isDialogOpen) {
        // Dialog was closed without saving
        if (cancelCallback) {
            cancelCallback();
        }
        reset();
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
        displayName =
            (std::strlen(tableNameBuffer) > 0 ? std::string(tableNameBuffer) : "New Table");
    } else {
        displayName =
            (std::strlen(tableNameBuffer) > 0 ? std::string(tableNameBuffer) : originalTable.name);
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
        std::format("   Columns ({})      ", editingTable.columns.size());
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
    ImU32 plusColor = isPlusHovered ? ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.3f, 1.0f))
                                    : ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.6f, 0.8f));

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
        for (size_t i = 0; i < editingTable.columns.size(); i++) {
            const auto& column = editingTable.columns[i];

            ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

            if (selectedColumnIndex == static_cast<int>(i)) {
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

            ImGui::PushID(static_cast<int>(i));
            ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);

            if (ImGui::IsItemClicked()) {
                startEditColumn(static_cast<int>(i));
                rightPanelMode = RightPanelMode::ColumnEditor;
            }

            // Context menu for individual column
            if (ImGui::BeginPopupContextItem("column_context_menu")) {
                if (ImGui::MenuItem("Edit Column")) {
                    startEditColumn(static_cast<int>(i));
                    rightPanelMode = RightPanelMode::ColumnEditor;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Column")) {
                    // Remove from local list
                    editingTable.columns.erase(editingTable.columns.begin() +
                                               static_cast<ptrdiff_t>(i));
                    if (selectedColumnIndex == static_cast<int>(i)) {
                        rightPanelMode = RightPanelMode::TableProperties;
                        columnEditMode = ColumnEditMode::None;
                        selectedColumnIndex = -1;
                        resetColumnForm();
                    } else if (selectedColumnIndex > static_cast<int>(i)) {
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
        for (const auto& column : editingTable.columns) {
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
                    std::strncpy(columnType, type.c_str(), sizeof(columnType) - 1);
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
    if (databaseType == DatabaseType::MYSQL || databaseType == DatabaseType::POSTGRESQL) {
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
        if (validateTableInput()) {
            Table resultTable = buildResultTable();
            errorMessage.clear();

            // Direct execution mode (dbNode set)
            if (dbNode) {
                if (dialogMode == TableDialogMode::Create) {
                    Logger::info("Creating table: " + resultTable.name);
                    auto [success, error] = dbNode->createTable(resultTable);
                    if (success) {
                        dbNode->startTablesLoadAsync(true);
                    } else {
                        errorMessage = error;
                    }
                } else {
                    // Edit mode - execute ALTER TABLE statements
                    auto statements = generateAlterTableStatements();
                    for (const auto& sql : statements) {
                        Logger::info("Executing: " + sql);
                        auto r = dbNode->executeQuery(sql);
                        auto success = !r.empty() && r[0].success;
                        auto error = r.empty() ? std::string("No result") : r[0].errorMessage;
                        if (!success) {
                            errorMessage = error;
                            break;
                        }
                    }
                    if (errorMessage.empty()) {
                        dbNode->startTablesLoadAsync(true);
                    }
                }
            }

            // Only close if no error was set
            if (errorMessage.empty()) {
                isDialogOpen = false;
                cancelCallback = nullptr;
                dbNode = nullptr;
                reset();
            }
        }
    }

    ImGui::PopStyleColor(3);
    ImGui::SameLine();

    // Close button
    ImGui::PushStyleColor(ImGuiCol_Button, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.overlay1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay2);

    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        isDialogOpen = false;
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

    editingTable.columns.push_back(newColumn);

    // Set up the editor for the new column
    columnEditMode = ColumnEditMode::Edit;
    selectedColumnIndex = static_cast<int>(editingTable.columns.size() - 1);
    rightPanelMode = RightPanelMode::ColumnEditor;

    // Populate the form with the new column data
    populateColumnFormFromColumn(newColumn);
    errorMessage.clear();
    updatePreviewSQL();
}

void TableDialog::startEditColumn(int columnIndex) {
    if (columnIndex >= 0 && columnIndex < static_cast<int>(editingTable.columns.size())) {
        columnEditMode = ColumnEditMode::Edit;
        selectedColumnIndex = columnIndex;
        rightPanelMode = RightPanelMode::ColumnEditor;
        originalColumnName = editingTable.columns[columnIndex].name;
        populateColumnFormFromColumn(editingTable.columns[columnIndex]);
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

void TableDialog::resetColumnForm() {
    std::memset(columnName, 0, sizeof(columnName));
    std::memset(columnType, 0, sizeof(columnType));
    std::memset(columnComment, 0, sizeof(columnComment));
    std::memset(defaultValue, 0, sizeof(defaultValue));

    isPrimaryKey = false;
    isNotNull = false;
    isUnique = false;
}

void TableDialog::populateColumnFormFromColumn(const Column& column) {
    std::strncpy(columnName, column.name.c_str(), sizeof(columnName) - 1);
    std::strncpy(columnType, column.type.c_str(), sizeof(columnType) - 1);
    std::strncpy(columnComment, column.comment.c_str(), sizeof(columnComment) - 1);

    isPrimaryKey = column.isPrimaryKey;
    isNotNull = column.isNotNull;
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

std::string TableDialog::generateAddColumnSQL() const {
    std::string tableName = std::strlen(tableNameBuffer) > 0 ? tableNameBuffer : originalTable.name;

    // For PostgreSQL, ensure table name is schema-qualified
    std::string qualifiedTableName = tableName;
    if (databaseType == DatabaseType::POSTGRESQL) {
        if (qualifiedTableName.find('.') == std::string::npos) {
            const std::string schema = schemaName.empty() ? "public" : schemaName;
            qualifiedTableName = schema + "." + qualifiedTableName;
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

    if (std::strlen(defaultValue) > 0) {
        sql += " DEFAULT " + std::string(defaultValue);
    }

    // Handle comments differently for different databases
    if (std::strlen(columnComment) > 0) {
        if (databaseType == DatabaseType::MYSQL) {
            sql += " COMMENT '" + std::string(columnComment) + "'";
        } else if (databaseType == DatabaseType::POSTGRESQL) {
            sql += "; COMMENT ON COLUMN " + qualifiedTableName + "." + std::string(columnName) +
                   " IS '" + std::string(columnComment) + "'";
        }
    }

    return sql;
}

std::string TableDialog::generateEditColumnSQL() const {
    std::string tableName = std::strlen(tableNameBuffer) > 0 ? tableNameBuffer : originalTable.name;
    std::string sql;

    switch (databaseType) {
    case DatabaseType::POSTGRESQL: {
        std::string qualifiedTableName = tableName;
        if (qualifiedTableName.find('.') == std::string::npos) {
            const std::string schema = schemaName.empty() ? "public" : schemaName;
            qualifiedTableName = schema + "." + qualifiedTableName;
        }

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
        if (std::strlen(defaultValue) > 0) {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " SET DEFAULT " +
                                 std::string(defaultValue));
        } else {
            statements.push_back("ALTER TABLE " + qualifiedTableName + " ALTER COLUMN " +
                                 std::string(columnName) + " DROP DEFAULT");
        }

        // Handle comment
        if (std::strlen(columnComment) > 0) {
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
        sql = "ALTER TABLE " + tableName + " MODIFY COLUMN " + std::string(columnName) + " " +
              std::string(columnType);

        if (isNotNull) {
            sql += " NOT NULL";
        }

        if (std::strlen(defaultValue) > 0) {
            sql += " DEFAULT " + std::string(defaultValue);
        }

        if (std::strlen(columnComment) > 0) {
            sql += " COMMENT '" + std::string(columnComment) + "'";
        }
        break;

    case DatabaseType::SQLITE:
        sql = "-- SQLite doesn't support column modification directly";
        break;

    default:
        sql = "-- Column editing not supported for this database type";
        break;
    }

    return sql;
}

std::vector<std::string> TableDialog::getCommonDataTypes() const {
    std::vector<std::string> types;

    switch (databaseType) {
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

    // Table Name
    ImGui::Text("Table Name:");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##table_name", tableNameBuffer, sizeof(tableNameBuffer))) {
        editingTable.name = tableNameBuffer;
        if (showPreview) {
            updatePreviewSQL();
        }
    }
    ImGui::Spacing();

    // Comment (if supported by database)
    if (databaseType == DatabaseType::MYSQL || databaseType == DatabaseType::POSTGRESQL) {
        ImGui::Text("Comment:");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline("##table_comment", tableCommentBuffer,
                                      sizeof(tableCommentBuffer), ImVec2(0, 60))) {
            // Note: Table struct doesn't have comment field, but we store it for SQL generation
            if (showPreview) {
                updatePreviewSQL();
            }
        }
        ImGui::Spacing();
    }

    // Pop style
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::Spacing();

    // Instructions
    if (dialogMode == TableDialogMode::Create) {
        ImGui::TextWrapped(
            "Add columns to your table by clicking the + icon or right-clicking on 'Columns' in "
            "the tree on the left.");

        if (editingTable.columns.empty()) {
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
    if (std::strlen(tableNameBuffer) == 0) {
        errorMessage = "Table name cannot be empty";
        return false;
    }

    // Check if table has at least one column
    if (editingTable.columns.empty()) {
        errorMessage = "Table must have at least one column";
        return false;
    }

    return true;
}

bool TableDialog::validateColumnInput() {
    errorMessage.clear();

    // Check column name
    if (std::strlen(columnName) == 0) {
        errorMessage = "Column name cannot be empty";
        return false;
    }

    // Check data type
    if (std::strlen(columnType) == 0) {
        errorMessage = "Data type cannot be empty";
        return false;
    }

    return true;
}

std::string TableDialog::generateCreateTableSQL() const {
    if (editingTable.columns.empty()) {
        return "";
    }

    std::string qualifiedTableName = std::string(tableNameBuffer);
    if (databaseType == DatabaseType::POSTGRESQL) {
        if (qualifiedTableName.find('.') == std::string::npos) {
            std::string schema = schemaName.empty() ? "public" : schemaName;
            qualifiedTableName = schema + "." + qualifiedTableName;
        }
    }

    std::string sql = "CREATE TABLE " + qualifiedTableName + " (\n";

    // Add columns
    for (size_t i = 0; i < editingTable.columns.size(); ++i) {
        const auto& column = editingTable.columns[i];
        sql += "    " + column.name + " " + column.type;

        if (column.isNotNull) {
            sql += " NOT NULL";
        }

        if (!column.comment.empty() && databaseType == DatabaseType::MYSQL) {
            sql += " COMMENT '" + column.comment + "'";
        }

        if (i < editingTable.columns.size() - 1) {
            sql += ",";
        }
        sql += "\n";
    }

    // Add primary key constraint if any columns are marked as primary key
    std::vector<std::string> primaryKeyColumns;
    for (const auto& column : editingTable.columns) {
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

    // Add table comment for MySQL
    if (databaseType == DatabaseType::MYSQL && std::strlen(tableCommentBuffer) > 0) {
        sql += " COMMENT = '" + std::string(tableCommentBuffer) + "'";
    }

    return sql;
}

std::vector<std::string> TableDialog::generateAlterTableStatements() const {
    std::vector<std::string> statements;
    auto builder = createSQLBuilder(databaseType);

    std::string tableName = originalTable.name;
    if (databaseType == DatabaseType::POSTGRESQL && !schemaName.empty()) {
        tableName = schemaName + "." + originalTable.name;
    }

    // Find dropped columns (in original but not in editing)
    for (const auto& origCol : originalTable.columns) {
        bool found = false;
        for (const auto& editCol : editingTable.columns) {
            if (origCol.name == editCol.name) {
                found = true;
                break;
            }
        }
        if (!found) {
            statements.push_back(builder->dropColumn(tableName, origCol.name));
        }
    }

    // Find added columns (in editing but not in original)
    for (const auto& editCol : editingTable.columns) {
        bool found = false;
        for (const auto& origCol : originalTable.columns) {
            if (editCol.name == origCol.name) {
                found = true;
                break;
            }
        }
        if (!found) {
            statements.push_back(builder->addColumn(tableName, editCol));
        }
    }

    return statements;
}

void TableDialog::updateCurrentColumn() {
    if (selectedColumnIndex >= 0 &&
        selectedColumnIndex < static_cast<int>(editingTable.columns.size())) {
        auto& column = editingTable.columns[selectedColumnIndex];
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

Table TableDialog::buildResultTable() const {
    Table result = editingTable;
    result.name = std::string(tableNameBuffer);
    // Note: Table struct doesn't have comment field
    return result;
}

void TableDialog::reset() {
    columnEditMode = ColumnEditMode::None;
    selectedColumnIndex = -1;
    rightPanelMode = RightPanelMode::TableProperties;

    editingTable = Table{};
    originalTable = Table{};
    originalColumnName.clear();
    schemaName.clear();

    std::memset(tableNameBuffer, 0, sizeof(tableNameBuffer));
    std::memset(tableCommentBuffer, 0, sizeof(tableCommentBuffer));
    resetColumnForm();

    errorMessage.clear();
    previewSQL.clear();
    showPreview = false;

    cancelCallback = nullptr;
    dbNode = nullptr;
}
