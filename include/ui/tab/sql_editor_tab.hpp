#pragma once

#include "TextEditor.h"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

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
    }
    [[nodiscard]] const std::string& getSelectedSchemaName() const {
        return selectedSchemaName;
    }
    void setSelectedSchemaName(const std::string& schemaName) {
        selectedSchemaName = schemaName;
    }
    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
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
    TextEditor sqlEditor;

    // Structured query result for table display
    QueryResult queryResult;
    std::string queryError;
    std::chrono::milliseconds lastQueryDuration{0};

    // Async query execution state
    bool isExecutingQuery = false;
    std::future<void> queryExecutionFuture;
    std::atomic<bool> shouldCancelQuery{false};

    // Splitter state for resizing between editor and results
    float splitterPosition = 0.4f; // 40% for editor, 60% for results
    bool splitterActive = false;
    float totalContentHeight = 0.0f; // Store total height for consistent splitter calculation

    // Helper methods for async execution
    void startQueryExecutionAsync(const std::string& query);
    void checkQueryExecutionStatus();
    void cancelQueryExecution();

    // Render component helper methods
    void renderConnectionInfo();
    void renderConnectionInfoPostgres();
    void renderConnectionInfoMySQL();
    void renderConnectionInfoSQLite();
    void renderToolbar();
    void renderQueryResults() const;
    void renderSingleResult(const StatementResult& r, size_t index) const;

    // Switch the active database node (clears results, resets autocomplete)
    void switchNode(IDatabaseNode* newNode);
    void bindNode(IDatabaseNode* node);
    void syncBoundNodePointer();

    // Helper method for splitter
    bool renderVerticalSplitter(const char* id, float* position, float minSize1,
                                float minSize2) const;

    // Autocomplete
    void updateCompletionKeywords();
    bool completionKeywordsSet_ = false;
    int pendingEditorFocusFrames_ = 3;

    // Deferred database switch (PostgreSQL: waiting for schemas to load)
    std::string pendingDatabaseSwitch_;

    // AI Chat panel
    std::unique_ptr<AIChatState> aiChatState_;
    std::unique_ptr<AIChatPanel> aiChatPanel_;
    bool aiPanelVisible_ = false;
    float aiPanelWidth_ = 350.0f;

    void initAIPanel();
    void renderAIToggleStrip(float stripWidth, float availableHeight);
    void renderAIPanel(float panelWidth, float availableHeight);
};
