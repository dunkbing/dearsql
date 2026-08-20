#include "ui/tab/mongo_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "ai/ai_chat.hpp"
#include "application.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/ai_chat_panel.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/table_renderer.hpp"
#include "utils/button.hpp"
#include "utils/sentry_utils.hpp"
#include "utils/spinner.hpp"
#include "utils/splitter.hpp"
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
    queryExecutionOp_.cancel();
}

void MongoEditorTab::render() {
    const bool dark = Application::getInstance().isDarkTheme();
    editor_.SetPalette(
        dearsql::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));

    if (!completionKeywordsSet_) {
        updateCompletionKeywords();
    }

    checkQueryExecutionStatus();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);
    renderHeader();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);

    AISettingsDialog::instance().render();

    constexpr float toggleStripWidth = 28.0f;
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    totalContentHeight_ = ImGui::GetContentRegionAvail().y;

    const float panelContentWidth = aiPanelVisible_ ? aiPanelWidth_ : 0.0f;
    float editorAreaWidth = totalWidth - toggleStripWidth - panelContentWidth;
    editorAreaWidth = std::max(200.0f, editorAreaWidth);

    // left pane: editor + results
    if (ImGui::BeginChild("##mongo_left_pane", ImVec2(editorAreaWidth, totalContentHeight_),
                          false)) {
        float paneHeight = ImGui::GetContentRegionAvail().y;
        const float toolbarHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::S;
        const float editorHeight = paneHeight * splitterPosition_;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition_) - 6.0f - toolbarHeight;

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
        UIUtils::Splitter("##mongo_splitter", &splitterPosition_, totalContentHeight_, 100.0f,
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
                ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(colors.base));
                bg.w = 0.75f;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(contentStart, overlayEnd, ImGui::GetColorU32(bg));

                float cx = (contentStart.x + overlayEnd.x) * 0.5f;
                float cy = (contentStart.y + overlayEnd.y) * 0.5f;

                constexpr float spinnerRadius = 10.0f;
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - spinnerRadius, cy - spinnerRadius - Theme::Spacing::M));
                UIUtils::Spinner("##mongo_results_spinner", spinnerRadius, 2,
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

    // AI panel content (when open)
    if (aiPanelVisible_) {
        ImGui::SameLine(0, 0);
        renderAIPanel(panelContentWidth, totalContentHeight_);
    }

    // toggle strip on the far right (always visible)
    ImGui::SameLine(0, 0);
    renderAIToggleStrip(toggleStripWidth, totalContentHeight_);
}

void MongoEditorTab::renderHeader() const {
    if (!node_) {
        ImGui::Text("Query Editor (No database selected)");
        ImGui::Separator();
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.green));
    ImGui::Text(ICON_FA_DATABASE);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);
    ImGui::Text("%s", node_->getFullPath().c_str());

    ImGui::Separator();
}

void MongoEditorTab::renderToolbar() {
    if (queryExecutionOp_.isRunning()) {
        ImGui::BeginDisabled();
        UIUtils::Button(ICON_FA_PLAY " Run", UIUtils::ButtonVariant::Primary);
        ImGui::EndDisabled();

        ImGui::SameLine(0, Theme::Spacing::M);
        if (UIUtils::Button(LABEL_CANCEL)) {
            cancelQueryExecution();
        }
    } else {
        if (UIUtils::Button(ICON_FA_PLAY " Run", UIUtils::ButtonVariant::Primary)) {
            startQueryExecutionAsync(query_);
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        if (UIUtils::Button(ICON_FA_ALIGN_LEFT " Format")) {
            formatJSON();
        }
    }
}

void MongoEditorTab::renderQueryResults() const {
    if (queryResult_.empty()) {
        ImGui::Text("%s", LABEL_NO_RESULTS);
        return;
    }

    if (queryResult_.executionTimeMs > 0) {
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

    if (r.columnNames.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", r.message.c_str());
        return;
    }

    if (r.tableData.empty()) {
        ImGui::Text("%s", LABEL_NO_ROWS);
    } else {
        ImGui::Text("Rows: %zu", r.tableData.size());
        if (static_cast<int>(r.tableData.size()) >= MAX_QUERY_ROWS) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", LABEL_ROW_LIMIT);
        }
    }

    if (!r.tableData.empty()) {
        float tableHeight = std::max(ImGui::GetContentRegionAvail().y - 20.0f, 50.0f);

        TableRenderer::Config config;
        config.allowEditing = false;
        config.showRowNumbers = false;
        config.minHeight = tableHeight;

        TableRenderer tableRenderer(config);
        tableRenderer.setColumns(r.columnNames);
        tableRenderer.setData(r.tableData);

        std::string tableId = "MongoQueryResults_" + std::to_string(index);
        tableRenderer.render(tableId.c_str());
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
        queryResult_.statements.push_back(r);
        return;
    }

    MongoDBDatabaseNode* nodePtr = node_;
    queryExecutionOp_.startCancellable([query, nodePtr](const std::stop_token& stopToken) {
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
        });
    } catch (const std::exception& e) {
        queryError_ = "Error in async query execution: " + std::string(e.what());
    }
}

void MongoEditorTab::cancelQueryExecution() {
    queryExecutionOp_.cancel();
}

void MongoEditorTab::updateCompletionKeywords() {
    using CI = dearsql::TextEditor::CompletionItem;
    using CK = dearsql::TextEditor::CompletionKind;

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

    editor_.SetCompletionItems(std::move(items));
    completionKeywordsSet_ = true;
}

void MongoEditorTab::formatJSON() {
    std::string formatted = dearsql::TextEditor::FormatJSON(editor_.GetText());
    if (!formatted.empty()) {
        editor_.SetText(formatted);
        query_ = formatted;
    }
}

void MongoEditorTab::initAIPanel() {
    aiChatState_ = std::make_unique<AIChatState>(node_);
    aiChatPanel_ = std::make_unique<AIChatPanel>(aiChatState_.get());
    aiChatPanel_->setInsertCallback([this](const std::string& json) {
        std::string current = editor_.GetText();
        if (!current.empty() && current.back() != '\n') {
            current += "\n";
        }
        current += json;
        editor_.SetText(current);
        query_ = current;
    });
}

void MongoEditorTab::renderAIToggleStrip(float stripWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("MongoAIToggleStrip", ImVec2(stripWidth, availableHeight),
                          ImGuiChildFlags_None)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        // left borderline
        drawList->AddLine(stripPos, ImVec2(stripPos.x, stripPos.y + availableHeight),
                          ImGui::GetColorU32(colors.overlay0), 1.0f);

        // rotated "Assistant" label as clickable tab
        const char* label = "Assistant";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        constexpr float padding = 6.0f;
        const float buttonW = stripWidth;
        const float buttonH = textSize.x + padding * 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y));
        ImGui::InvisibleButton("##mongoToggleAI", ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            aiPanelVisible_ = !aiPanelVisible_;
            if (aiPanelVisible_ && !aiChatPanel_) {
                initAIPanel();
            }
        }

        // button background
        const ImVec2 btnMin = stripPos;
        const ImVec2 btnMax(stripPos.x + buttonW, stripPos.y + buttonH);
        if (aiPanelVisible_ || hovered) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        }

        // bottom border of button area
        drawList->AddLine(ImVec2(btnMin.x, btnMax.y), btnMax, ImGui::GetColorU32(colors.overlay0),
                          1.0f);

        // draw rotated text centered in the button area
        const float cx = stripPos.x + buttonW * 0.5f;
        const float cy = stripPos.y + buttonH * 0.5f;
        const float textX = cx - textSize.x * 0.5f;
        const float textY = cy - textSize.y * 0.5f;

        drawList->PushClipRectFullScreen();
        const int vtxBegin = drawList->VtxBuffer.Size;
        drawList->AddText(
            ImVec2(textX, textY),
            ImGui::GetColorU32(hovered || aiPanelVisible_ ? colors.text : colors.subtext0), label);
        const int vtxEnd = drawList->VtxBuffer.Size;

        // rotate all text vertices 90 degrees around center
        for (int i = vtxBegin; i < vtxEnd; i++) {
            ImDrawVert& v = drawList->VtxBuffer[i];
            const float dx = v.pos.x - cx;
            const float dy = v.pos.y - cy;
            v.pos.x = cx - dy;
            v.pos.y = cy + dx;
        }
        drawList->PopClipRect();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MongoEditorTab::renderAIPanel(float panelWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginChild("MongoAIPanel", ImVec2(panelWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        // resize handle on the left edge
        {
            constexpr float handleWidth = 4.0f;
            const ImVec2 panelPos = ImGui::GetWindowPos();
            const ImVec2 handleMin(panelPos.x, panelPos.y);

            ImGui::SetCursorScreenPos(handleMin);
            ImGui::InvisibleButton("##mongoAiResizeHandle", ImVec2(handleWidth, availableHeight));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                aiPanelWidth_ -= ImGui::GetIO().MouseDelta.x;
                aiPanelWidth_ = std::clamp(aiPanelWidth_, 250.0f, 600.0f);
            }

            ImGui::SetCursorPos(ImVec2(0, 0));
        }

        if (!aiChatPanel_) {
            initAIPanel();
        }
        if (aiChatState_) {
            aiChatState_->setCurrentSQL(query_);
        }
        if (aiChatPanel_) {
            aiChatPanel_->render();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
