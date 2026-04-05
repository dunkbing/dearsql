#pragma once

#include "app_state.hpp"
#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

// Forward declarations
class IDatabaseNode;
class IQueryExecutor;
class AIChatState;
class AIChatPanel;

class SQLEditorTab final : public Tab {
public:
    explicit SQLEditorTab(const std::string& name, IDatabaseNode* node,
                          const std::string& schemaName = "");

    ~SQLEditorTab() override;

    void render() override;

    // SQL Editor specific methods
    [[nodiscard]] const std::string& getQuery() const {
        return sqlQuery;
    }
    void setQuery(const std::string& query) {
        sqlQuery = query;
        sqlEditor.SetText(query);
        scheduleSyntaxCheck();
    }
    [[nodiscard]] const std::string& getSelectedSchemaName() const {
        return selectedSchemaName;
    }
    void setSelectedSchemaName(const std::string& schemaName) {
        selectedSchemaName = schemaName;
    }
    void setSelectedTable(const std::string& tableName) {
        selectedTableName_ = tableName;
    }
    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
    }

    // Script file association
    void loadFromScript(const SqlScript& script);
    [[nodiscard]] const std::string& getFilePath() const {
        return filePath_;
    }
    [[nodiscard]] int getScriptId() const {
        return scriptId_;
    }
    [[nodiscard]] bool hasUnsavedChanges() const override {
        return contentModified_;
    }

private:
    struct NodeBinding {
        std::function<IDatabaseNode*()> resolveNode;
        std::function<IQueryExecutor*()> resolveExecutor;
    };

    std::string sqlQuery;
    IDatabaseNode* node_ = nullptr; // Database node implementing IDatabaseNode
    NodeBinding binding_;
    std::string selectedSchemaName; // Selected schema within the database (for postgres)
    dearsql::TextEditor sqlEditor;

    // Structured query result for table display
    QueryResult queryResult;
    std::string queryError;
    std::chrono::milliseconds lastQueryDuration{0};

    // Async query execution state
    AsyncOperation<QueryResult> queryExecutionOp_;

    // Splitter state for resizing between editor and results
    float splitterPosition = 0.4f;
    float totalContentHeight = 0.0f;

    // Helper methods for async execution
    void startQueryExecutionAsync(const std::string& query);
    void checkQueryExecutionStatus();
    void cancelQueryExecution();

    // Render component helper methods
    void renderConnectionInfo();
    void renderConnectionInfoPostgres();
    void renderConnectionInfoMySQL();
    void renderConnectionInfoMSSQL();
    void renderConnectionInfoOracle();
    void renderConnectionInfoSQLite();
    void renderDatabaseCombo(const std::string& host, const char* label,
                             const std::string& currentName,
                             const std::vector<std::string>& dbNames,
                             const std::function<void(const std::string&)>& onSelect);
    void renderToolbar();
    void renderQueryResults() const;
    void renderSingleResult(const StatementResult& r, size_t index) const;

    // Switch the active database node (clears results, resets autocomplete)
    void switchNode(IDatabaseNode* newNode);
    void bindNode(IDatabaseNode* node);
    void syncBoundNodePointer();

    // Formatting
    void formatSQL();
    void scheduleSyntaxCheck();
    void updateSyntaxDiagnostics();

    // Autocomplete
    void updateCompletionKeywords();
    bool completionKeywordsSet_ = false;
    int pendingEditorFocusFrames_ = 3;

    struct SyntaxDiagnostic {
        bool active = false;
        bool advisory = true;
        int line = 0;
        int column = 0;
        std::string message;
    };
    SyntaxDiagnostic syntaxDiagnostic_;
    float syntaxCheckDelay_ = 0.0f;
    bool syntaxCheckPending_ = false;

    // Deferred database switch (PostgreSQL: waiting for schemas to load)
    std::string pendingDatabaseSwitch_;
    std::string selectedTableName_; // The currently selected table in the UI

    // AI Chat panel
    std::unique_ptr<AIChatState> aiChatState_;
    std::unique_ptr<AIChatPanel> aiChatPanel_;
    bool aiPanelVisible_ = false;
    float aiPanelWidth_ = 350.0f;

    void initAIPanel();
    void renderAIToggleStrip(float stripWidth, float availableHeight);
    void renderAIPanel(float panelWidth, float availableHeight);

    // Script persistence
    int scriptId_ = 0;
    std::string filePath_;
    std::string scriptName_;
    bool contentModified_ = false;
    bool renamingScript_ = false;
    bool renamingFocusNeeded_ = false; // request focus on the first rename frame
    char renameBuffer_[256] = {};

    void saveScript();
    void renderScriptHeader();
    [[nodiscard]] static std::string getDefaultScriptsDir();
    void persistScriptToAppState();
};
