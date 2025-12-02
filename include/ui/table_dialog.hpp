#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <functional>
#include <string>
#include <vector>

enum class ColumnEditMode { None, Add, Edit };
enum class TableDialogMode { Edit, Create };
enum class RightPanelMode { TableProperties, ColumnEditor, Instructions };

/**
 * @brief A reusable table editor dialog component (singleton)
 *
 * Use this for creating or editing table structures.
 * The dialog receives a Table and returns the modified Table via callback.
 */
class TableDialog {
public:
    using SaveCallback = std::function<void(const Table&)>;
    using CancelCallback = std::function<void()>;

    // Singleton access
    static TableDialog& instance();

    // Delete copy/move
    TableDialog(const TableDialog&) = delete;
    TableDialog& operator=(const TableDialog&) = delete;
    TableDialog(TableDialog&&) = delete;
    TableDialog& operator=(TableDialog&&) = delete;

    /**
     * @brief Show dialog for creating a new table
     *
     * @param databaseType The type of database (for syntax/type hints)
     * @param schemaName Optional schema name for display
     * @param onSave Callback with the created Table when user confirms
     * @param onCancel Optional callback when user cancels
     */
    void showCreate(DatabaseType databaseType, const std::string& schemaName, SaveCallback onSave,
                    CancelCallback onCancel = nullptr);

    /**
     * @brief Show dialog for editing an existing table
     *
     * @param table The table to edit
     * @param databaseType The type of database (for syntax/type hints)
     * @param schemaName Optional schema name for display
     * @param onSave Callback with the modified Table when user confirms
     * @param onCancel Optional callback when user cancels
     */
    void showEdit(const Table& table, DatabaseType databaseType, const std::string& schemaName,
                  SaveCallback onSave, CancelCallback onCancel = nullptr);

    // Render the dialog (call from UI loop)
    void render();

    // Check if dialog is open
    bool isOpen() const {
        return isDialogOpen;
    }

    // Set an error message to display in the dialog
    void setError(const std::string& error) {
        errorMessage = error;
    }

    // Clear the error message
    void clearError() {
        errorMessage.clear();
    }

private:
    TableDialog() = default;
    ~TableDialog() = default;

    // Dialog state
    bool isDialogOpen = false;
    TableDialogMode dialogMode = TableDialogMode::Create;
    ColumnEditMode columnEditMode = ColumnEditMode::None;
    RightPanelMode rightPanelMode = RightPanelMode::TableProperties;

    // Database context
    DatabaseType databaseType = DatabaseType::SQLITE;
    std::string schemaName;

    // Table being edited
    Table editingTable;
    std::string originalTableName; // For edit mode comparison

    // Column editing state
    int selectedColumnIndex = -1;
    std::string originalColumnName; // For column edit mode

    // Table properties fields
    char tableNameBuffer[256] = "";
    char tableCommentBuffer[512] = "";

    // Form fields for column editing
    char columnName[256] = "";
    char columnType[256] = "";
    char columnComment[512] = "";
    bool isPrimaryKey = false;
    bool isNotNull = false;
    bool isUnique = false;
    char defaultValue[256] = "";

    // Callbacks
    SaveCallback saveCallback;
    CancelCallback cancelCallback;

    // Error handling
    std::string errorMessage;
    std::string previewSQL;

    // UI state
    bool showPreview = false;

    // Rendering
    void renderLeftPanel();
    void renderRightPanel();
    void renderTableTree();
    void renderColumnsNode();
    void renderKeysNode() const;
    void renderColumnEditor();
    void renderTableProperties();
    static void renderInstructions();
    void renderPreviewPanel() const;
    void renderButtons();

    // Column operations
    void startAddColumn();
    void startEditColumn(int columnIndex);
    void cancelColumnEdit();
    void updateCurrentColumn();

    // Table operations
    bool validateTableInput();
    std::string generateCreateTableSQL() const;

    // Validation
    bool validateColumnInput();

    // Helper methods
    void reset();
    void resetColumnForm();
    void populateColumnFormFromColumn(const Column& column);
    void updatePreviewSQL();
    std::string generateAddColumnSQL() const;
    std::string generateEditColumnSQL() const;
    [[nodiscard]] std::vector<std::string> getCommonDataTypes() const;

    // Build the result table from current state
    Table buildResultTable() const;
};
