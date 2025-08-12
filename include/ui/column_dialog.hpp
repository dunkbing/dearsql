#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <memory>
#include <string>

enum class ColumnDialogMode { Add, Edit };

class ColumnDialog {
public:
    ColumnDialog() = default;
    ~ColumnDialog() = default;

    // Show dialog for adding a new column
    void showAddColumnDialog(const std::shared_ptr<DatabaseInterface> &db,
                             const std::string &tableName, const std::string &schemaName = "");

    // Show dialog for editing an existing column
    void showEditColumnDialog(const std::shared_ptr<DatabaseInterface> &db,
                              const std::string &tableName, const Column &column,
                              const std::string &schemaName = "");

    // Check if dialog is currently open
    bool isDialogOpen() const {
        return isOpen;
    }

    // Get the result (will be nullptr if dialog cancelled or not completed)
    bool hasResult() const {
        return hasCompletedResult;
    }
    void clearResult() {
        hasCompletedResult = false;
    }

    // Render the dialog (call this from UI loop)
    void renderDialog();

private:
    // Dialog state
    bool isOpen = false;
    bool hasCompletedResult = false;
    ColumnDialogMode mode = ColumnDialogMode::Add;

    // Database context
    std::shared_ptr<DatabaseInterface> database;
    std::string targetTableName;
    std::string targetSchemaName;   // For PostgreSQL schema qualification
    std::string originalColumnName; // For edit mode

    // Form fields
    char columnName[256] = "";
    char columnType[256] = "";
    char columnComment[512] = "";
    bool isPrimaryKey = false;
    bool isNotNull = false;
    bool isUnique = false;
    char defaultValue[256] = "";

    // Error handling
    std::string errorMessage;

    // Rendering
    void renderFormFields();
    void renderButtons();

    // Validation and execution
    bool validateInput();
    bool executeAddColumn();
    bool executeEditColumn();

    // Helper methods
    void resetForm();
    void populateFormFromColumn(const Column &column);
    std::string generateAddColumnSQL();
    std::string generateEditColumnSQL();
    std::vector<std::string> getCommonDataTypes();
};
