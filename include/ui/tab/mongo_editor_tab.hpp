#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <chrono>
#include "ui/table_renderer.hpp"
#include <memory>
#include <string>

class MongoDBDatabaseNode;
class AIChatState;
class AIChatPanel;

class MongoEditorTab final : public Tab {
public:
    explicit MongoEditorTab(const std::string& name, MongoDBDatabaseNode* node);
    ~MongoEditorTab() override;

    void render() override;

    [[nodiscard]] MongoDBDatabaseNode* getDatabaseNode() const {
        return node_;
    }

private:
    std::string query_;
    MongoDBDatabaseNode* node_ = nullptr;
    dearsql::TextEditor editor_;

    // query result
    QueryResult queryResult_;
    // one grid per result, built when it first shows and dropped with the result
    mutable std::vector<std::unique_ptr<TableRenderer>> resultGrids_;
    std::string queryError_;
    std::chrono::milliseconds lastQueryDuration_{0};

    // async execution
    AsyncOperation<QueryResult> queryExecutionOp_;

    // splitter
    float splitterPosition_ = 0.4f;
    float totalContentHeight_ = 0.0f;
    int pendingEditorFocusFrames_ = 3;

    void startQueryExecutionAsync(const std::string& query);
    void checkQueryExecutionStatus();
    void cancelQueryExecution();

    void renderHeader() const;
    void renderToolbar();
    void renderQueryResults() const;
    void renderSingleResult(const StatementResult& r, size_t index) const;

    void formatJSON();

    void updateCompletionKeywords();
    bool completionKeywordsSet_ = false;

    // AI panel
    std::unique_ptr<AIChatState> aiChatState_;
    std::unique_ptr<AIChatPanel> aiChatPanel_;
    bool aiPanelVisible_ = false;
    float aiPanelWidth_ = 350.0f;

    void initAIPanel();
    void renderAIToggleStrip(float stripWidth, float availableHeight);
    void renderAIPanel(float panelWidth, float availableHeight);
};
