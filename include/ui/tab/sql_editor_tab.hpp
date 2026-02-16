#pragma once

#include "TextEditor.h"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

// Forward declarations
class IDatabaseNode;

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
    [[nodiscard]] const std::string& getResult() const {
        return queryResult;
    }
    void setResult(const std::string& result) {
        queryResult = result;
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
    std::string sqlQuery;
    std::string queryResult;
    IDatabaseNode* node_ = nullptr; // Database node implementing IDatabaseNode
    std::string selectedSchemaName; // Selected schema within the database (for postgres)
    TextEditor sqlEditor;

    // Structured query results for table display (one per statement)
    std::vector<QueryResult> queryResults;
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
    void renderToolbar();
    void renderQueryResults() const;
    void renderSingleResult(const QueryResult& r, size_t index) const;

    // Helper method for splitter
    bool renderVerticalSplitter(const char* id, float* position, float minSize1,
                                float minSize2) const;

    // Autocomplete
    void updateCompletionKeywords();
    bool completionKeywordsSet_ = false;
};
