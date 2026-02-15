#include "ui/tab/sql_editor_tab.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "imgui.h"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <future>

namespace {
    constexpr const char* LABEL_EXECUTING = "Executing...";
    constexpr const char* LABEL_RUNNING_QUERY = "Running query...";
    constexpr const char* LABEL_CANCEL = "Cancel";
    constexpr const char* LABEL_EXECUTE_QUERY = "Execute Query";
    constexpr const char* LABEL_CLEAR = "Clear";
    constexpr const char* LABEL_NO_DATABASE = "SQL Editor (No database selected)";
    constexpr const char* LABEL_NO_ROWS = "No rows returned.";
    constexpr const char* LABEL_ROW_LIMIT = "(limited to 1000 rows)";
    constexpr const char* LABEL_QUERY_SUCCESS = "Query executed successfully.";
    constexpr const char* LABEL_NO_RESULTS =
        "No results to display. Execute a query to see results here.";
    constexpr const char* LABEL_QUERY_CANCELLED = "Query execution cancelled by user";
    constexpr const char* LABEL_NO_DATABASE_SELECTED = "No database selected";
    constexpr int MAX_QUERY_ROWS = 1000;
} // namespace

SQLEditorTab::SQLEditorTab(const std::string& name, IDatabaseNode* node,
                           const std::string& schemaName)
    : Tab(name, TabType::SQL_EDITOR), node_(node), selectedSchemaName(schemaName) {
    sqlEditor.SetLanguage(TextEditor::Language::Sql());
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);
}

SQLEditorTab::~SQLEditorTab() {
    if (isExecutingQuery && queryExecutionFuture.valid()) {
        queryExecutionFuture.wait();
    }
}

void SQLEditorTab::render() {
    if (node_ && !completionKeywordsSet_ && node_->isTablesLoaded()) {
        updateCompletionKeywords();
    }

    checkQueryExecutionStatus();

    renderConnectionInfo();

    totalContentHeight = ImGui::GetContentRegionAvail().y;
    const float editorHeight = totalContentHeight * splitterPosition;
    const float resultsHeight = totalContentHeight * (1.0f - splitterPosition) - 6.0f;

    if (ImGui::BeginChild("SQLEditor", ImVec2(-1, editorHeight), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        sqlEditor.Render("##SQL", ImVec2(-1, -1), true);
        sqlQuery = sqlEditor.GetText();
    }
    ImGui::EndChild();

    renderVerticalSplitter("##sql_splitter", &splitterPosition, 100.0f, 200.0f);

    if (ImGui::BeginChild("SQLResults", ImVec2(-1, resultsHeight), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        renderToolbar();
        renderQueryResults();
    }
    ImGui::EndChild();
}

void SQLEditorTab::renderConnectionInfo() {
    if (node_) {
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
    } else {
        ImGui::Text("%s", LABEL_NO_DATABASE);
    }
    ImGui::Separator();
}

void SQLEditorTab::renderToolbar() {
    if (isExecutingQuery) {
        ImGui::BeginDisabled();
        ImGui::Button(LABEL_EXECUTING);
        ImGui::EndDisabled();

        ImGui::SameLine();
        UIUtils::Spinner("##query_spinner", 8.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));

        ImGui::SameLine();
        ImGui::Text("%s", LABEL_RUNNING_QUERY);

        ImGui::SameLine();
        if (ImGui::Button(LABEL_CANCEL)) {
            cancelQueryExecution();
        }
    } else {
        if (ImGui::Button(LABEL_EXECUTE_QUERY)) {
            startQueryExecutionAsync(sqlQuery);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(LABEL_CLEAR)) {
        sqlEditor.SetText("");
        sqlQuery.clear();
        hasStructuredResults = false;
        queryColumnNames.clear();
        queryTableData.clear();
        queryError.clear();
    }

    ImGui::Separator();
}

void SQLEditorTab::renderQueryResults() const {
    if (!queryError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", queryError.c_str());
    } else if (hasStructuredResults && !queryColumnNames.empty()) {
        if (queryTableData.empty()) {
            ImGui::Text("%s", LABEL_NO_ROWS);
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %ld ms",
                            static_cast<long>(lastQueryDuration.count()));
            }
        } else {
            ImGui::Text("Rows: %zu", queryTableData.size());
            if (queryTableData.size() >= MAX_QUERY_ROWS) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", LABEL_ROW_LIMIT);
            }
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %ld ms",
                            static_cast<long>(lastQueryDuration.count()));
            }

            float tableAvailableHeight = ImGui::GetContentRegionAvail().y - 20.0f;
            tableAvailableHeight = std::max(tableAvailableHeight, 50.0f);

            TableRenderer::Config config;
            config.allowEditing = false;
            config.allowSelection = true;
            config.showRowNumbers = false;
            config.minHeight = tableAvailableHeight;

            TableRenderer tableRenderer(config);
            tableRenderer.setColumns(queryColumnNames);
            tableRenderer.setData(queryTableData);

            tableRenderer.render("QueryResults");
        }
    } else if (hasStructuredResults && queryColumnNames.empty()) {
        ImGui::Text("%s", LABEL_QUERY_SUCCESS);
        if (lastQueryDuration.count() > 0) {
            ImGui::SameLine();
            ImGui::Text("| Execution time: %ld ms", static_cast<long>(lastQueryDuration.count()));
        }
    } else {
        ImGui::Text("%s", LABEL_NO_RESULTS);
    }
}

void SQLEditorTab::startQueryExecutionAsync(const std::string& query) {
    if (isExecutingQuery) {
        return;
    }

    isExecutingQuery = true;
    shouldCancelQuery = false;
    hasStructuredResults = false;
    queryError.clear();
    queryColumnNames.clear();
    queryTableData.clear();
    lastQueryDuration = std::chrono::milliseconds{0};

    IDatabaseNode* executor = node_;

    queryExecutionFuture = std::async(std::launch::async, [this, query, executor]() {
        if (shouldCancelQuery) {
            return;
        }

        QueryResult result;

        if (executor) {
            result = executor->executeQueryWithResult(query);
        } else {
            result.success = false;
            result.errorMessage = LABEL_NO_DATABASE_SELECTED;
        }

        if (shouldCancelQuery) {
            return;
        }

        lastQueryDuration = std::chrono::milliseconds{result.executionTimeMs};

        if (result.success) {
            queryColumnNames = result.columnNames;
            queryTableData = result.tableData;
            hasStructuredResults = true;
            queryError.clear();
            queryResult.clear();
        } else {
            queryResult = result.errorMessage;
            queryError = result.errorMessage;
            hasStructuredResults = false;
            queryColumnNames.clear();
            queryTableData.clear();
        }
    });
}

void SQLEditorTab::checkQueryExecutionStatus() {
    if (!isExecutingQuery) {
        return;
    }

    if (queryExecutionFuture.valid() &&
        queryExecutionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            queryExecutionFuture.get();
        } catch (const std::exception& e) {
            if (!shouldCancelQuery) {
                queryResult = "Error in async query execution: " + std::string(e.what());
                queryError = queryResult;
                hasStructuredResults = false;
                queryColumnNames.clear();
                queryTableData.clear();
            }
        }

        isExecutingQuery = false;
    }
}

void SQLEditorTab::cancelQueryExecution() {
    shouldCancelQuery = true;
    queryResult = LABEL_QUERY_CANCELLED;
    queryError = queryResult;
    hasStructuredResults = false;
    queryColumnNames.clear();
    queryTableData.clear();
}

bool SQLEditorTab::renderVerticalSplitter(const char* id, float* position, float minSize1,
                                          float minSize2) const {
    constexpr float hoverThickness = 6.0f;

    ImGui::InvisibleButton(id, ImVec2(-1, hoverThickness));

    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    if (hovered || held) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    bool changed = false;
    if (held) {
        const float delta = ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f) {
            const float availableHeight = totalContentHeight;
            const float currentPixelPos = *position * availableHeight;
            const float newPixelPos = currentPixelPos + delta;
            float newPosition = newPixelPos / availableHeight;

            const float minPos1 = minSize1 / availableHeight;
            const float maxPos1 = 1.0f - (minSize2 / availableHeight);

            newPosition = std::max(minPos1, std::min(maxPos1, newPosition));

            if (newPosition != *position) {
                *position = newPosition;
                changed = true;
            }
        }
    }

    // Draw splitter line
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetItemRectMin();
    const ImVec2 size = ImGui::GetItemRectSize();
    const float centerY = pos.y + size.y / 2.0f;
    const ImU32 color = (hovered || held) ? ImGui::GetColorU32(ImGuiCol_SeparatorHovered)
                                          : ImGui::GetColorU32(ImGuiCol_Separator);
    drawList->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + size.x, centerY), color, 2.0f);

    return changed;
}

void SQLEditorTab::updateCompletionKeywords() {
    if (!node_)
        return;

    std::vector<std::string> keywords;

    for (const auto& table : node_->getTables()) {
        keywords.push_back(table.name);
        for (const auto& col : table.columns) {
            keywords.push_back(col.name);
        }
    }

    if (node_->isViewsLoaded()) {
        for (const auto& view : node_->getViews()) {
            keywords.push_back(view.name);
        }
    }

    for (const auto& seq : node_->getSequences()) {
        keywords.push_back(seq);
    }

    std::sort(keywords.begin(), keywords.end());
    keywords.erase(std::unique(keywords.begin(), keywords.end()), keywords.end());

    sqlEditor.SetCompletionKeywords(keywords);
    completionKeywordsSet_ = true;
}
