#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <functional>
#include <string>
#include <vector>

class IDatabaseNode;

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
     * @brief Show dialog for creating a new table (direct execution)
     *
     * @param dbNode The database node to execute CREATE TABLE on
     * @param schemaName Optional schema name for qualified table names
     */
    void showCreate(IDatabaseNode* dbNode, const std::string& schemaName = "");

    /**
     * @brief Show dialog for editing an existing table (direct execution)
     *
     * @param dbNode The database node to execute ALTER TABLE on
     * @param table The table to edit
     * @param schemaName Optional schema name for qualified table names
     */
    void showEdit(IDatabaseNode* dbNode, const Table& table, const std::string& schemaName = "");

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
    IDatabaseNode* dbNode = nullptr;

    // Table being edited
    Table editingTable;
    Table originalTable; // For edit mode comparison (full copy)

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
    std::vector<std::string> generateAlterTableStatements() const;

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
