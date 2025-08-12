#include "ui/column_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/log_panel.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

void ColumnDialog::showAddColumnDialog(const std::shared_ptr<DatabaseInterface> &db,
                                       const std::string &tableName,
                                       const std::string &schemaName) {
    database = db;
    targetTableName = tableName;
    targetSchemaName = schemaName;
    mode = ColumnDialogMode::Add;
    originalColumnName = "";

    resetForm();
    isOpen = true;
    hasCompletedResult = false;
    errorMessage.clear();
}

void ColumnDialog::showEditColumnDialog(const std::shared_ptr<DatabaseInterface> &db,
                                        const std::string &tableName, const Column &column,
                                        const std::string &schemaName) {
    database = db;
    targetTableName = tableName;
    targetSchemaName = schemaName;
    mode = ColumnDialogMode::Edit;
    originalColumnName = column.name;

    resetForm();
    populateFormFromColumn(column);
    isOpen = true;
    hasCompletedResult = false;
    errorMessage.clear();
}

void ColumnDialog::renderDialog() {
    if (!isOpen)
        return;

    const char *title = (mode == ColumnDialogMode::Add) ? "Add Column" : "Edit Column";

    // Always try to open the popup when dialog is active
    if (!ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
    }

    // Set popup size
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal(title, &isOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Show table context
        ImGui::Text("Table: %s", targetTableName.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        renderFormFields();

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
        resetForm();
        errorMessage.clear();
    }
}

void ColumnDialog::renderFormFields() {
    // Push style for input fields with borders
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

    // Column Name
    ImGui::Text("Column Name:");
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("##column_name", columnName, sizeof(columnName));

    ImGui::Spacing();

    // Data Type with editable dropdown
    ImGui::Text("Data Type:");
    ImGui::SetNextItemWidth(300);

    // Set content size before opening combo to prevent width animation
    ImGui::SetNextWindowContentSize(ImVec2(250, 0));

    // Create an editable combo box
    if (ImGui::BeginCombo("##column_type", columnType, ImGuiComboFlags_None)) {
        // Add input field at the top for filtering/editing
        ImGui::SetNextItemWidth(-1);

        // Set focus to the input field when combo opens
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        if (ImGui::InputText("##type_filter", columnType, sizeof(columnType))) {
            // Input changed, we can add filtering logic here if needed
        }

        ImGui::Separator();

        auto commonTypes = getCommonDataTypes();
        std::string currentInput = std::string(columnType);

        // Filter types based on current input (case-insensitive)
        for (const auto &type : commonTypes) {
            std::string lowerType = type;
            std::string lowerInput = currentInput;
            std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
            std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(), ::tolower);

            if (lowerInput.empty() || lowerType.find(lowerInput) != std::string::npos) {
                bool isSelected = (type == currentInput);
                if (ImGui::Selectable(type.c_str(), isSelected)) {
                    strncpy(columnType, type.c_str(), sizeof(columnType) - 1);
                    columnType[sizeof(columnType) - 1] = '\0';
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
    ImGui::Checkbox("Primary Key", &isPrimaryKey);
    ImGui::SameLine();
    ImGui::Checkbox("NOT NULL", &isNotNull);
    ImGui::SameLine();
    ImGui::Checkbox("UNIQUE", &isUnique);

    ImGui::Spacing();

    // Default Value
    ImGui::Text("Default Value (optional):");
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("##default_value", defaultValue, sizeof(defaultValue));

    ImGui::Spacing();

    // Comment (if supported by database)
    if (database->getType() == DatabaseType::MYSQL ||
        database->getType() == DatabaseType::POSTGRESQL) {
        ImGui::Text("Comment:");
        ImGui::SetNextItemWidth(350);
        ImGui::InputTextMultiline("##column_comment", columnComment, sizeof(columnComment),
                                  ImVec2(0, 60));
    }

    // Pop style
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void ColumnDialog::renderButtons() {
    const char *actionText = (mode == ColumnDialogMode::Add) ? "Add Column" : "Update Column";

    const auto &colors = Application::getInstance().getCurrentColors();

    // button colors
    ImGui::PushStyleColor(ImGuiCol_Button, colors.blue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.sky);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.sapphire);

    if (ImGui::Button(actionText, ImVec2(120, 0))) {
        if (validateInput()) {
            bool success = false;
            if (mode == ColumnDialogMode::Add) {
                success = executeAddColumn();
            } else {
                success = executeEditColumn();
            }

            if (success) {
                hasCompletedResult = true;
                isOpen = false;
            }
        }
    }

    ImGui::PopStyleColor(3); // Pop the 3 button colors

    ImGui::SameLine();

    // cancel button colors
    ImGui::PushStyleColor(ImGuiCol_Button, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.overlay1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay2);

    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        isOpen = false;
    }

    ImGui::PopStyleColor(3); // Pop the 3 button colors
}

bool ColumnDialog::validateInput() {
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

    // Skip column name validation for now - let the database handle duplicate column errors
    // The cached table structure might not be up to date

    return true;
}

bool ColumnDialog::executeAddColumn() {
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
                std::cout << "Add column result: " << result1 << std::endl;
                if (result1.find("ERROR") != std::string::npos ||
                    result1.find("Error") != std::string::npos) {
                    // Extract the actual error message
                    std::string cleanError = result1;
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
            std::cout << "Add column result: " << result << std::endl;

            // Check if there was an error in the result
            if (result.find("ERROR") != std::string::npos ||
                result.find("Error") != std::string::npos) {
                // Extract the actual error message
                std::string cleanError = result;
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

    } catch (const std::exception &e) {
        errorMessage = "Failed to add column: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}

bool ColumnDialog::executeEditColumn() {
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
            for (const auto &statement : statements) {
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

    } catch (const std::exception &e) {
        errorMessage = "Failed to edit column: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}

std::string ColumnDialog::generateAddColumnSQL() {
    // For PostgreSQL, ensure table name is schema-qualified
    std::string qualifiedTableName = targetTableName;
    if (database->getType() == DatabaseType::POSTGRESQL) {
        // If table name doesn't already contain a schema prefix, add schema
        if (qualifiedTableName.find('.') == std::string::npos) {
            std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
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

std::string ColumnDialog::generateEditColumnSQL() {
    std::string sql;

    // Different databases have different syntax for altering columns
    switch (database->getType()) {
    case DatabaseType::POSTGRESQL: {
        std::string qualifiedTableName = targetTableName;
        if (qualifiedTableName.find('.') == std::string::npos) {
            std::string schemaName = targetSchemaName.empty() ? "public" : targetSchemaName;
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

void ColumnDialog::resetForm() {
    memset(columnName, 0, sizeof(columnName));
    memset(columnType, 0, sizeof(columnType));
    memset(columnComment, 0, sizeof(columnComment));
    memset(defaultValue, 0, sizeof(defaultValue));

    isPrimaryKey = false;
    isNotNull = false;
    isUnique = false;
}

void ColumnDialog::populateFormFromColumn(const Column &column) {
    strncpy(columnName, column.name.c_str(), sizeof(columnName) - 1);
    strncpy(columnType, column.type.c_str(), sizeof(columnType) - 1);
    strncpy(columnComment, column.comment.c_str(), sizeof(columnComment) - 1);

    isPrimaryKey = column.isPrimaryKey;
    isNotNull = column.isNotNull;
}

std::vector<std::string> ColumnDialog::getCommonDataTypes() {
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
