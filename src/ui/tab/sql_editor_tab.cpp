#include "ui/tab/sql_editor_tab.hpp"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "imgui.h"
#include "ui/table_renderer.hpp"
#include "utils/sentry_utils.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
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
    // Sync editor palette with current app theme
    const bool dark = Application::getInstance().isDarkTheme();
    sqlEditor.SetPalette(dark ? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette());

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
        queryResults.clear();
        queryError.clear();
    }

    ImGui::Separator();
}

void SQLEditorTab::renderQueryResults() const {
    if (queryResults.empty()) {
        ImGui::Text("%s", LABEL_NO_RESULTS);
        return;
    }

    // Single result — render directly without tabs
    if (queryResults.size() == 1) {
        const auto& r = queryResults[0];
        renderSingleResult(r, 0);
        return;
    }

    // Multiple results — render as tabs
    if (ImGui::BeginTabBar("##QueryResultTabs")) {
        int tabIndex = 0;
        for (size_t i = 0; i < queryResults.size(); ++i) {
            const auto& r = queryResults[i];

            std::string tabLabel;
            if (!r.success) {
                tabLabel = std::format("Error##{}", i);
            } else if (r.columnNames.empty()) {
                tabLabel = std::format("Result {}##{}", tabIndex + 1, i);
            } else {
                tabLabel = std::format("Result {}##{}", tabIndex + 1, i);
            }
            ++tabIndex;

            if (ImGui::BeginTabItem(tabLabel.c_str())) {
                renderSingleResult(r, i);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void SQLEditorTab::renderSingleResult(const QueryResult& r, size_t index) const {
    if (!r.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", r.errorMessage.c_str());
        return;
    }

    if (r.columnNames.empty()) {
        // DML/DDL result
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", r.message.c_str());
        if (r.executionTimeMs > 0) {
            ImGui::SameLine();
            ImGui::Text("| Execution time: %lld ms", r.executionTimeMs);
        }
        return;
    }

    // SELECT result
    if (r.tableData.empty()) {
        ImGui::Text("%s", LABEL_NO_ROWS);
    } else {
        ImGui::Text("Rows: %zu", r.tableData.size());
        if (static_cast<int>(r.tableData.size()) >= MAX_QUERY_ROWS) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", LABEL_ROW_LIMIT);
        }
    }

    if (r.executionTimeMs > 0) {
        ImGui::SameLine();
        ImGui::Text("| Execution time: %lld ms", r.executionTimeMs);
    }

    if (!r.tableData.empty()) {
        float tableHeight = std::max(ImGui::GetContentRegionAvail().y - 20.0f, 50.0f);

        TableRenderer::Config config;
        config.allowEditing = false;
        config.allowSelection = true;
        config.showRowNumbers = false;
        config.minHeight = tableHeight;

        TableRenderer tableRenderer(config);
        tableRenderer.setColumns(r.columnNames);
        tableRenderer.setData(r.tableData);

        std::string tableId = "QueryResults_" + std::to_string(index);
        tableRenderer.render(tableId.c_str());
    }
}

void SQLEditorTab::startQueryExecutionAsync(const std::string& query) {
    if (isExecutingQuery) {
        return;
    }

    isExecutingQuery = true;
    shouldCancelQuery = false;
    queryError.clear();
    queryResults.clear();
    lastQueryDuration = std::chrono::milliseconds{0};

    IDatabaseNode* executor = node_;

    queryExecutionFuture = std::async(std::launch::async, [this, query, executor]() {
        if (shouldCancelQuery) {
            return;
        }

        std::vector<QueryResult> results;

        if (executor) {
            results = executor->executeQuery(query);
        } else {
            QueryResult r;
            r.success = false;
            r.errorMessage = LABEL_NO_DATABASE_SELECTED;
            results.push_back(r);
        }

        if (shouldCancelQuery) {
            return;
        }

        // Check for any errors
        for (const auto& r : results) {
            if (!r.success) {
                queryError = r.errorMessage;
                SentryUtils::addBreadcrumb("query", "Query error", "error", r.errorMessage,
                                           "error");
                break;
            }
        }

        if (!results.empty()) {
            lastQueryDuration = std::chrono::milliseconds{results.back().executionTimeMs};
        }

        queryResults = std::move(results);
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
                queryResults.clear();
            }
        }

        isExecutingQuery = false;
    }
}

void SQLEditorTab::cancelQueryExecution() {
    shouldCancelQuery = true;
    queryResult = LABEL_QUERY_CANCELLED;
    queryError = queryResult;
    queryResults.clear();
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
