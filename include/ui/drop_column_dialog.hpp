#pragma once

#include "database/db_interface.hpp"
#include <memory>
#include <string>

class DropColumnDialog {
public:
    DropColumnDialog() = default;
    ~DropColumnDialog() = default;

    // Show confirmation dialog for dropping a column
    void showDropColumnDialog(const std::shared_ptr<DatabaseInterface> &db,
                              const std::string &tableName, const std::string &columnName);

    // Check if dialog is currently open
    bool isDialogOpen() const {
        return isOpen;
    }

    // Get the result
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

    // Database context
    std::shared_ptr<DatabaseInterface> database;
    std::string targetTableName;
    std::string targetColumnName;

    // Error handling
    std::string errorMessage;

    // Execution
    bool executeDropColumn();
    std::string generateDropColumnSQL();
};
