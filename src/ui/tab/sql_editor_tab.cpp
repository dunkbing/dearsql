#include "ui/tab/sql_editor_tab.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "imgui.h"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>

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
        ImGui::Text("SQL Editor (No database selected)");
    }
    ImGui::Separator();
}

void SQLEditorTab::renderToolbar() {
    if (isExecutingQuery) {
        ImGui::BeginDisabled();
        ImGui::Button("Executing...");
        ImGui::EndDisabled();

        ImGui::SameLine();
        UIUtils::Spinner("##query_spinner", 8.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));

        ImGui::SameLine();
        ImGui::Text("Running query...");

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            cancelQueryExecution();
        }
    } else {
        if (ImGui::Button("Execute Query")) {
            startQueryExecutionAsync(sqlQuery);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
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
            ImGui::Text("No rows returned.");
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %ld ms",
                            static_cast<long>(lastQueryDuration.count()));
            }
        } else {
            ImGui::Text("Rows: %zu", queryTableData.size());
            if (queryTableData.size() >= 1000) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(limited to 1000 rows)");
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
        ImGui::Text("Query executed successfully.");
        if (lastQueryDuration.count() > 0) {
            ImGui::SameLine();
            ImGui::Text("| Execution time: %ld ms", static_cast<long>(lastQueryDuration.count()));
        }
    } else {
        ImGui::Text("No results to display. Execute a query to see results here.");
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
            result.errorMessage = "No database selected";
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
    queryResult = "Query execution cancelled by user";
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
