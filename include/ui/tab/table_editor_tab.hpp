#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "ui/auto_complete_input.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <memory>
#include <string>
#include <vector>

class IDatabaseNode;

enum class ColumnEditMode { None, Add, Edit };
enum class TableEditorMode { Edit, Create };
enum class RightPanelMode { TableProperties, ColumnEditor, Instructions };

class TableEditorTab final : public Tab {
public:
    TableEditorTab(IDatabaseNode* node, const std::string& schema);
    TableEditorTab(IDatabaseNode* node, const Table& table, const std::string& schema);

    void render() override;
    [[nodiscard]] bool hasUnsavedChanges() const override {
        return dirty;
    }

    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return dbNode;
    }

private:
    TableEditorMode editorMode = TableEditorMode::Create;
    ColumnEditMode columnEditMode = ColumnEditMode::None;
    bool focusColumnName = false;
    RightPanelMode rightPanelMode = RightPanelMode::TableProperties;

    DatabaseType databaseType = DatabaseType::SQLITE;
    std::string schemaName;
    IDatabaseNode* dbNode = nullptr;

    Table editingTable;
    Table originalTable;

    int selectedColumnIndex = -1;
    std::string originalColumnName;

    char tableNameBuffer[256] = "";
    char tableCommentBuffer[512] = "";
    char columnName[256] = "";
    char columnType[256] = "";
    char columnComment[512] = "";
    bool isPrimaryKey = false;
    bool isNotNull = false;
    bool isUnique = false;
    bool isAutoIncrement = false;
    char defaultValue[256] = "";

    std::string errorMessage;
    dearsql::TextEditor previewEditor;
    std::unique_ptr<AutoCompleteInput> columnTypeAutoComplete;
    bool showPreviewPopup = false;
    bool dirty = false;
    float leftPanelWidth = 300.0f;

    void renderContent(bool& closeRequested);
    void renderLeftPanel();
    void renderRightPanel();
    void renderTableTree();
    void renderColumnsNode();
    void renderKeysNode() const;
    void renderColumnEditor();
    void renderTableProperties();
    static void renderInstructions();
    void renderPreviewPopup(bool& closeRequested);
    void renderButtons(bool& closeRequested);

    void startAddColumn();
    void startEditColumn(int columnIndex);
    void cancelColumnEdit();
    void updateCurrentColumn();

    bool validateTableInput();
    bool validateColumnInput();
    std::string generateCreateTableSQL() const;
    std::string generateAddColumnSQL() const;
    std::string generateEditColumnSQL() const;
    std::vector<std::string> generateAlterTableStatements() const;
    [[nodiscard]] std::vector<std::string> getCommonDataTypes() const;
    void initializeColumnTypeAutoComplete();

    void markDirty();
    void reset();
    void resetColumnForm();
    void populateColumnFormFromColumn(const Column& column);
    Table buildResultTable() const;
};
