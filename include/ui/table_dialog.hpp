#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <variant>

// Forward declarations
class PostgresSchemaNode;
class MySQLDatabaseNode;
class SQLiteDatabase;

enum class ColumnEditMode { None, Add, Edit };
enum class TableDialogMode { Edit, Create };
enum class RightPanelMode { TableProperties, ColumnEditor, Instructions };

class TableDialog {
public:
    using DatabaseNode =
        std::variant<std::monostate, PostgresSchemaNode*, MySQLDatabaseNode*, SQLiteDatabase*>;

    TableDialog() = default;
    ~TableDialog() = default;

    // Show dialog for editing a table
    void showTableDialog(const DatabaseNode& dbNode, const std::string& tableName,
                         const std::string& schemaName = "");

    // Show dialog for creating a new table
    void showCreateTableDialog(const DatabaseNode& dbNode, const std::string& schemaName = "");

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
    TableDialogMode dialogMode = TableDialogMode::Edit;
    ColumnEditMode columnEditMode = ColumnEditMode::None;
    RightPanelMode rightPanelMode = RightPanelMode::Instructions;

    // Database context
    DatabaseNode databaseNode;
    std::string targetTableName;
    std::string targetSchemaName;
    std::vector<Column> tableColumns;
    int selectedColumnIndex = -1;
    std::string originalColumnName; // For edit mode

    // Table creation fields
    char newTableName[256] = "";
    char newTableComment[512] = "";

    // Table editing fields
    char editTableName[256] = "";
    char editTableComment[512] = "";

    // Form fields for column editing
    char columnName[256] = "";
    char columnType[256] = "";
    char columnComment[512] = "";
    bool isPrimaryKey = false;
    bool isNotNull = false;
    bool isUnique = false;
    char defaultValue[256] = "";

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
    bool saveColumn();
    void updateCurrentColumn();

    // Table operations
    void renderTableCreationForm();
    bool validateTableInput();
    bool executeCreateTable();
    bool saveTableChanges();
    std::string generateCreateTableSQL();
    std::string generateAddColumnSQLForColumn(const Column& column);
    std::string generateEditColumnSQLForColumn(const Column& column,
                                               const std::string& originalName);

    // Validation and execution
    bool validateColumnInput();
    bool executeAddColumn();
    bool executeEditColumn();

    // Helper methods
    void resetColumnForm();
    void populateColumnFormFromColumn(const Column& column);
    void loadTableStructure();
    void updatePreviewSQL();
    std::string generateAddColumnSQL();
    std::string generateEditColumnSQL();
    [[nodiscard]] std::vector<std::string> getCommonDataTypes() const;
    [[nodiscard]] DatabaseType getDatabaseType() const;
    std::string executeQuery(const std::string& query);
    const std::vector<Table>& getTables() const;
};
