#include "ui/tab/mongo_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "imgui.h"
#include "imshell/button.hpp"
#include "imshell/spinner.hpp"
#include "imshell/splitter.hpp"
#include "themes.hpp"
#include "ui/table_renderer.hpp"
#include "utils/sentry_utils.hpp"
#include <algorithm>
#include <chrono>
#include <format>

namespace {
    constexpr const char* LABEL_RUNNING_QUERY = "Running query...";
    constexpr const char* LABEL_CANCEL = "Cancel";
    constexpr const char* LABEL_NO_ROWS = "No rows returned.";
    constexpr const char* LABEL_ROW_LIMIT = "(limited to 1000 rows)";
    constexpr const char* LABEL_NO_RESULTS =
        "No results to display. Execute a query to see results here.";
    constexpr int MAX_QUERY_ROWS = 1000;
} // namespace

MongoEditorTab::MongoEditorTab(const std::string& name, MongoDBDatabaseNode* node)
    : Tab(name, TabType::MONGO_EDITOR), node_(node) {
    editor_.SetShowLineNumbers(true);
    editor_.SetLanguage(dearsql::TextEditor::Language::JSON);
    editor_.SetPlaceholder("{ \"collection\": \"users\", \"command\": \"find\", \"filter\": {} }\n"
                           "\n"
                           "Commands: find, aggregate, insert, update, delete,\n"
                           "          createCollection, dropCollection, runCommand");
    editor_.SetSubmitCallback([this] {
        query_ = editor_.GetText();
        startQueryExecutionAsync(query_);
    });
}

MongoEditorTab::~MongoEditorTab() {
    // never join a query on close: the task holds only the query and the node,
    // and a node being destroyed waits out calls still running on it
    if (queryExecutionOp_.isRunning())
        ConnectionPoolBase::cancelQueriesOn(queryExecutionOp_.workerId());
    queryExecutionOp_.detach();
}

void MongoEditorTab::render() {
    editor_.SetPalette(
        dearsql::TextEditor::FromTheme(Application::getInstance().getCurrentColors()));

    if (!completionKeywordsSet_) {
        updateCompletionKeywords();
    }

    checkQueryExecutionStatus();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);
    renderHeader();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);

    const float editorAreaWidth = ImGui::GetContentRegionAvail().x;
    totalContentHeight_ = ImGui::GetContentRegionAvail().y;

    // left pane: editor + results
    if (ImGui::BeginChild("##mongo_left_pane", ImVec2(editorAreaWidth, totalContentHeight_),
                          false)) {
        float paneHeight = ImGui::GetContentRegionAvail().y;
        const float toolbarHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::S;
        const float editorHeight = paneHeight * splitterPosition_;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition_) - 4.0f - toolbarHeight;

        if (ImGui::BeginChild("MongoEditor", ImVec2(-1, editorHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            if (pendingEditorFocusFrames_ > 0) {
                editor_.SetFocus();
                pendingEditorFocusFrames_--;
            }
            editor_.Render("##Mongo", ImVec2(-1, -1), true);
            query_ = editor_.GetText();
        }
        ImGui::EndChild();

        renderToolbar();
        imshell::Splitter("##mongo_splitter", &splitterPosition_, totalContentHeight_, 100.0f,
                          200.0f);

        if (ImGui::BeginChild("MongoResults", ImVec2(-1, resultsHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            ImVec2 contentStart = ImGui::GetCursorScreenPos();
            const bool isRunning = queryExecutionOp_.isRunning();
            if (isRunning)
                ImGui::BeginDisabled();
            renderQueryResults();
            if (isRunning)
                ImGui::EndDisabled();

            if (isRunning) {
                ImVec2 winPos = ImGui::GetWindowPos();
                ImVec2 winSize = ImGui::GetWindowSize();
                ImVec2 overlayEnd(winPos.x + winSize.x, winPos.y + winSize.y);

                const auto& colors = Application::getInstance().getCurrentColors();
                ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(colors.background));
                bg.w = 0.75f;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(contentStart, overlayEnd, ImGui::GetColorU32(bg));

                float cx = (contentStart.x + overlayEnd.x) * 0.5f;
                float cy = (contentStart.y + overlayEnd.y) * 0.5f;

                constexpr float spinnerRadius = 10.0f;
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - spinnerRadius, cy - spinnerRadius - Theme::Spacing::M));
                imshell::Spinner("##mongo_results_spinner", spinnerRadius, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));

                const char* loadingText = LABEL_RUNNING_QUERY;
                ImVec2 textSize = ImGui::CalcTextSize(loadingText);
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - textSize.x * 0.5f, cy + spinnerRadius + Theme::Spacing::S));
                ImGui::Text("%s", loadingText);
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void MongoEditorTab::renderHeader() const {
    if (!node_) {
        ImGui::Text("Query Editor (No database selected)");
        ImGui::Separator();
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.success));
    ImGui::Text(ICON_FA_DATABASE);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);
    ImGui::Text("%s", node_->getFullPath().c_str());

    ImGui::Separator();
}

void MongoEditorTab::renderToolbar() {
    if (queryExecutionOp_.isRunning()) {
        ImGui::BeginDisabled();
        imshell::Button(ICON_FA_PLAY " Run", imshell::ButtonVariant::Primary);
        ImGui::EndDisabled();

        ImGui::SameLine(0, Theme::Spacing::M);
        if (imshell::Button(LABEL_CANCEL)) {
            cancelQueryExecution();
        }
    } else {
        if (imshell::Button(ICON_FA_PLAY " Run", imshell::ButtonVariant::Primary)) {
            startQueryExecutionAsync(query_);
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        if (imshell::Button(ICON_FA_ALIGN_LEFT " Format")) {
            formatJSON();
        }
    }
}

void MongoEditorTab::renderQueryResults() const {
    if (queryResult_.empty()) {
        ImGui::Text("%s", LABEL_NO_RESULTS);
        return;
    }

    // one result shows its time on its row-count line
    if (queryResult_.executionTimeMs > 0 && !(queryResult_.size() == 1)) {
        ImGui::Text("Execution time: %.2f ms", queryResult_.executionTimeMs);
    }

    if (queryResult_.size() == 1) {
        renderSingleResult(queryResult_[0], 0);
        return;
    }

    if (ImGui::BeginTabBar("##MongoQueryResultTabs")) {
        int tabIndex = 0;
        for (size_t i = 0; i < queryResult_.size(); ++i) {
            const auto& r = queryResult_[i];
            std::string tabLabel;
            if (!r.success) {
                tabLabel = std::format("Error##{}", i);
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

void MongoEditorTab::renderSingleResult(const StatementResult& r, size_t index) const {
    if (!r.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", r.errorMessage.c_str());
        return;
    }

    // "347 rows  0.53 ms": the time rides along when this is the only result
    const std::string took = queryResult_.size() == 1 && queryResult_.executionTimeMs > 0
                                 ? std::format("  {:.2f} ms", queryResult_.executionTimeMs)
                                 : "";

    if (r.columnNames.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s%s", r.message.c_str(), took.c_str());
        return;
    }

    if (r.tableData.empty()) {
        ImGui::Text("%s%s", LABEL_NO_ROWS, took.c_str());
        return;
    }

    // the grid, then "347 rows  0.53 ms" under it
    {
        float tableHeight = std::max(
            ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing(), 50.0f);

        if (resultGrids_.size() <= index)
            resultGrids_.resize(index + 1);
        auto& grid = resultGrids_[index];
        if (!grid) {
            TableRenderer::Config config;
            config.allowEditing = false;
            config.showRowNumbers = false;
            grid = std::make_unique<TableRenderer>(config);
            grid->setColumns(r.columnNames);
            grid->setData(r.tableData);
        }

        std::string tableId = "MongoQueryResults_" + std::to_string(index);
        // the grid fills what it is given, so give it all but the status line

        if (ImGui::BeginChild((tableId + "_grid").c_str(), ImVec2(0, tableHeight))) {

            grid->render(tableId.c_str());
        }

        ImGui::EndChild();
    }
    ImGui::Text("%zu %s%s", r.tableData.size(), r.tableData.size() == 1 ? "row" : "rows",
                took.c_str());
    if (static_cast<int>(r.tableData.size()) >= MAX_QUERY_ROWS) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", LABEL_ROW_LIMIT);
    }
}

void MongoEditorTab::startQueryExecutionAsync(const std::string& query) {
    if (queryExecutionOp_.isRunning())
        return;

    queryError_.clear();
    lastQueryDuration_ = std::chrono::milliseconds{0};

    if (!node_) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "No database selected";
        queryResult_ = QueryResult{};
        resultGrids_.clear();
        queryResult_.statements.push_back(r);
        return;
    }

    MongoDBDatabaseNode* nodePtr = node_;
    queryExecutionOp_.startCancellable(
        [query, nodePtr, keep = keepOwnerAlive(nodePtr)](const std::stop_token& stopToken) {
            QueryResult result;
            if (stopToken.stop_requested())
                return result;
            result = nodePtr->executeQuery(query);
            if (stopToken.stop_requested())
                return QueryResult{};
            return result;
        });

    // show placeholder while running
    StatementResult r;
    r.success = false;
    r.errorMessage = "Executing...";
    queryResult_ = QueryResult{};
    resultGrids_.clear();
    queryResult_.statements.push_back(r);
}

void MongoEditorTab::checkQueryExecutionStatus() {
    try {
        queryExecutionOp_.check([this](QueryResult result) {
            if (!result.empty() && !result.success()) {
                queryError_ = result.errorMessage();
                SentryUtils::addBreadcrumb("query", "Query error", "error", queryError_, "error");
            }
            lastQueryDuration_ =
                std::chrono::milliseconds{static_cast<long long>(result.executionTimeMs)};
            queryResult_ = std::move(result);
            resultGrids_.clear();
        });
    } catch (const std::exception& e) {
        queryError_ = "Error in async query execution: " + std::string(e.what());
    }
}

void MongoEditorTab::cancelQueryExecution() {
    queryExecutionOp_.cancel();
}

void MongoEditorTab::updateCompletionKeywords() {
    using CI = dearsql::ListCompletion;
    using CK = dearsql::CompletionKind;

    // MongoDB shell keywords and aggregation operators
    static const std::vector<std::string> mongoKeywords = {
        // common methods
        "find",
        "findOne",
        "insertOne",
        "insertMany",
        "updateOne",
        "updateMany",
        "deleteOne",
        "deleteMany",
        "aggregate",
        "count",
        "countDocuments",
        "estimatedDocumentCount",
        "distinct",
        "createIndex",
        "dropIndex",
        "getIndexes",
        "drop",
        "rename",
        "replaceOne",
        "bulkWrite",
        // aggregation stages
        "$match",
        "$group",
        "$sort",
        "$project",
        "$limit",
        "$skip",
        "$unwind",
        "$lookup",
        "$addFields",
        "$set",
        "$unset",
        "$replaceRoot",
        "$replaceWith",
        "$merge",
        "$out",
        "$count",
        "$facet",
        "$bucket",
        "$bucketAuto",
        "$sample",
        "$sortByCount",
        "$graphLookup",
        "$redact",
        "$geoNear",
        // query operators
        "$eq",
        "$ne",
        "$gt",
        "$gte",
        "$lt",
        "$lte",
        "$in",
        "$nin",
        "$and",
        "$or",
        "$not",
        "$nor",
        "$exists",
        "$type",
        "$regex",
        "$text",
        "$where",
        "$all",
        "$elemMatch",
        "$size",
        // update operators
        "$set",
        "$unset",
        "$inc",
        "$mul",
        "$rename",
        "$min",
        "$max",
        "$push",
        "$pull",
        "$addToSet",
        "$pop",
        "$each",
        "$slice",
        "$position",
        // accumulator operators
        "$sum",
        "$avg",
        "$first",
        "$last",
        "$min",
        "$max",
        "$stdDevPop",
        "$stdDevSamp",
        // js keywords
        "var",
        "let",
        "const",
        "function",
        "return",
        "if",
        "else",
        "for",
        "while",
        "true",
        "false",
        "null",
        "undefined",
        "new",
        "this",
        "db",
        "ObjectId",
        "ISODate",
        "NumberLong",
        "NumberInt",
        "NumberDecimal",
    };

    std::vector<CI> items;
    for (const auto& kw : mongoKeywords)
        items.push_back({kw, CK::Keyword});

    // add collection names as table completions
    if (node_) {
        for (const auto& coll : node_->getTables())
            items.push_back({coll.name, CK::Table});
    }

    // sort and deduplicate
    std::ranges::sort(items, [](const CI& a, const CI& b) { return a.text < b.text; });
    auto ret =
        std::ranges::unique(items, [](const CI& a, const CI& b) { return a.text == b.text; });
    items.erase(ret.begin(), ret.end());

    editor_.SetCompletionProvider(dearsql::listCompletions(std::move(items)));
    completionKeywordsSet_ = true;
}

void MongoEditorTab::formatJSON() {
    std::string formatted = dearsql::TextEditor::FormatJSON(editor_.GetText());
    if (!formatted.empty()) {
        editor_.SetText(formatted);
        query_ = formatted;
    }
}
