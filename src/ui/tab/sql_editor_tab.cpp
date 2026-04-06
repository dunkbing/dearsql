#include "ui/tab/sql_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "SQLParser.h"
#include "ai/ai_chat.hpp"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/ai_chat_panel.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/table_renderer.hpp"
#include "utils/sentry_utils.hpp"
#include "utils/spinner.hpp"
#include "utils/splitter.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <spdlog/spdlog.h>

namespace {
    constexpr const char* LABEL_RUNNING_QUERY = "Running query...";
    constexpr const char* LABEL_CANCEL = "Cancel";
    constexpr const char* LABEL_NO_DATABASE = "SQL Editor (No database selected)";
    constexpr const char* LABEL_NO_ROWS = "No rows returned.";
    constexpr const char* LABEL_ROW_LIMIT = "(limited to 1000 rows)";
    constexpr const char* LABEL_NO_RESULTS =
        "No results to display. Execute a query to see results here.";
    constexpr const char* LABEL_NO_DATABASE_SELECTED = "No database selected";
    constexpr int MAX_QUERY_ROWS = 1000;

    using CompletionItem = dearsql::TextEditor::CompletionItem;
    using CompletionKind = dearsql::TextEditor::CompletionKind;

    std::string joinQualifiers(const std::vector<std::string>& qualifiers) {
        std::string joined;
        for (size_t i = 0; i < qualifiers.size(); ++i) {
            if (i > 0)
                joined += ".";
            joined += qualifiers[i];
        }
        return joined;
    }

    CompletionItem makeCompletionItem(std::string label, CompletionKind kind,
                                      std::vector<std::string> qualifiers = {}) {
        CompletionItem item(std::move(label), kind);
        item.qualifiers = std::move(qualifiers);
        item.detailText = joinQualifiers(item.qualifiers);
        item.matchText = item.detailText.empty() ? item.text : item.detailText + "." + item.text;
        item.insertText = item.matchText;
        return item;
    }

    void sortAndDeduplicateCompletionItems(std::vector<CompletionItem>& items) {
        std::ranges::sort(items, [](const CompletionItem& a, const CompletionItem& b) {
            if (a.text != b.text)
                return a.text < b.text;
            if (a.detailText != b.detailText)
                return a.detailText < b.detailText;
            if (a.matchText != b.matchText)
                return a.matchText < b.matchText;
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        });
        auto ret = std::ranges::unique(items, [](const CompletionItem& a, const CompletionItem& b) {
            return a.text == b.text && a.detailText == b.detailText && a.matchText == b.matchText &&
                   a.kind == b.kind;
        });
        items.erase(ret.begin(), ret.end());
    }

    void scheduleMetadataLoad(IDatabaseNode* node) {
        if (!node)
            return;

        node->checkLoadingStatus();
        if (!node->isTablesLoaded() && !node->isLoadingTables())
            node->startTablesLoadAsync();
        if (!node->isViewsLoaded() && !node->isLoadingViews())
            node->startViewsLoadAsync();
    }

    std::string toLowerCopy(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (const char ch : s)
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        return out;
    }

    enum class SqlCompletionContext { Other, FromJoin, SelectWhereOn };

    SqlCompletionContext
    detectSqlCompletionContext(const dearsql::TextEditor::CompletionRequest& request) {
        const int wordStartPos = request.cursorIndex - static_cast<int>(request.currentWord.size());
        int p = wordStartPos;
        while (p > 0 && (request.content[static_cast<size_t>(p - 1)] == ' ' ||
                         request.content[static_cast<size_t>(p - 1)] == '\n' ||
                         request.content[static_cast<size_t>(p - 1)] == '\t' ||
                         request.content[static_cast<size_t>(p - 1)] == ',' ||
                         request.content[static_cast<size_t>(p - 1)] == '\r')) {
            --p;
        }

        const int kwEnd = p;
        while (p > 0) {
            const char ch = request.content[static_cast<size_t>(p - 1)];
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
                break;
            --p;
        }

        if (kwEnd <= p)
            return SqlCompletionContext::Other;

        const std::string kw = toLowerCopy(
            request.content.substr(static_cast<size_t>(p), static_cast<size_t>(kwEnd - p)));
        if (kw == "from" || kw == "join" || kw == "table" || kw == "into" || kw == "update")
            return SqlCompletionContext::FromJoin;
        if (kw == "select" || kw == "where" || kw == "and" || kw == "or" || kw == "on" ||
            kw == "having" || kw == "by" || kw == "set" || kw == "case" || kw == "when" ||
            kw == "then") {
            return SqlCompletionContext::SelectWhereOn;
        }
        return SqlCompletionContext::Other;
    }

    std::set<std::string>
    extractReferencedTables(const dearsql::TextEditor::CompletionRequest& request) {
        std::set<std::string> referencedTables;

        int stmtStart = 0;
        for (int p = request.cursorIndex - 1; p >= 0; --p) {
            if (request.content[static_cast<size_t>(p)] == ';') {
                stmtStart = p + 1;
                break;
            }
        }

        const std::string stmt = std::string(request.content.substr(
            static_cast<size_t>(stmtStart), static_cast<size_t>(request.cursorIndex - stmtStart)));
        const std::string lowerStmt = toLowerCopy(stmt);

        auto extractTablesAfterKeyword = [&](std::string_view keyword) {
            size_t searchPos = 0;
            while ((searchPos = lowerStmt.find(keyword, searchPos)) != std::string::npos) {
                if (searchPos > 0 &&
                    std::isalnum(static_cast<unsigned char>(lowerStmt[searchPos - 1]))) {
                    searchPos += keyword.size();
                    continue;
                }

                size_t afterKw = searchPos + keyword.size();
                if (afterKw < lowerStmt.size() &&
                    std::isalnum(static_cast<unsigned char>(lowerStmt[afterKw]))) {
                    searchPos = afterKw;
                    continue;
                }

                size_t p = afterKw;
                while (p < stmt.size() &&
                       (stmt[p] == ' ' || stmt[p] == '\n' || stmt[p] == '\t' || stmt[p] == '\r')) {
                    ++p;
                }

                while (p < stmt.size()) {
                    while (p < stmt.size() && (stmt[p] == ' ' || stmt[p] == '\n' ||
                                               stmt[p] == '\t' || stmt[p] == '\r')) {
                        ++p;
                    }
                    if (p >= stmt.size())
                        break;

                    size_t kwCheck = p;
                    std::string nextWord;
                    while (kwCheck < stmt.size() &&
                           (std::isalnum(static_cast<unsigned char>(stmt[kwCheck])) ||
                            stmt[kwCheck] == '_')) {
                        nextWord.push_back(static_cast<char>(
                            std::tolower(static_cast<unsigned char>(stmt[kwCheck++]))));
                    }

                    if (nextWord == "where" || nextWord == "on" || nextWord == "set" ||
                        nextWord == "order" || nextWord == "group" || nextWord == "having" ||
                        nextWord == "limit" || nextWord == "union" || nextWord == "inner" ||
                        nextWord == "left" || nextWord == "right" || nextWord == "outer" ||
                        nextWord == "cross" || nextWord == "full" || nextWord == "join" ||
                        nextWord == "select" || nextWord == "values") {
                        break;
                    }

                    std::string tableName;
                    while (p < stmt.size() && (std::isalnum(static_cast<unsigned char>(stmt[p])) ||
                                               stmt[p] == '_' || stmt[p] == '.')) {
                        tableName.push_back(stmt[p++]);
                    }

                    if (!tableName.empty()) {
                        referencedTables.insert(tableName);
                        const auto dotPos = tableName.rfind('.');
                        if (dotPos != std::string::npos)
                            referencedTables.insert(tableName.substr(dotPos + 1));
                    }

                    while (p < stmt.size() &&
                           (stmt[p] == ' ' || stmt[p] == '\n' || stmt[p] == '\t')) {
                        ++p;
                    }
                    if (p < stmt.size() && stmt[p] != ',' && stmt[p] != ';') {
                        std::string maybeAlias;
                        while (
                            p < stmt.size() &&
                            (std::isalnum(static_cast<unsigned char>(stmt[p])) || stmt[p] == '_')) {
                            maybeAlias.push_back(static_cast<char>(
                                std::tolower(static_cast<unsigned char>(stmt[p++]))));
                        }
                        if (maybeAlias == "as") {
                            while (p < stmt.size() && (stmt[p] == ' ' || stmt[p] == '\t'))
                                ++p;
                            while (p < stmt.size() &&
                                   (std::isalnum(static_cast<unsigned char>(stmt[p])) ||
                                    stmt[p] == '_')) {
                                ++p;
                            }
                        }
                    }

                    while (p < stmt.size() && (stmt[p] == ' ' || stmt[p] == '\n' ||
                                               stmt[p] == '\t' || stmt[p] == '\r')) {
                        ++p;
                    }
                    if (p < stmt.size() && stmt[p] == ',')
                        ++p;
                    else
                        break;
                }

                searchPos = afterKw;
            }
        };

        extractTablesAfterKeyword("from");
        extractTablesAfterKeyword("join");
        extractTablesAfterKeyword("update");
        return referencedTables;
    }

    std::vector<CompletionItem>
    filterSqlCompletions(const dearsql::TextEditor::CompletionRequest& request,
                         const std::vector<CompletionItem>& items) {
        const std::string lowerWord = toLowerCopy(request.currentWord);
        std::vector<std::string> lowerQualifierParts;
        lowerQualifierParts.reserve(request.qualifierParts.size());
        for (const auto& part : request.qualifierParts)
            lowerQualifierParts.push_back(toLowerCopy(part));

        const auto ctx = detectSqlCompletionContext(request);
        const auto referencedTables = extractReferencedTables(request);
        const bool hasTableContext = !referencedTables.empty();

        auto qualifiersMatch = [&](const CompletionItem& ci) {
            if (lowerQualifierParts.empty())
                return true;

            if (!ci.qualifiers.empty()) {
                if (ci.qualifiers.size() < lowerQualifierParts.size())
                    return false;
                const size_t offset = ci.qualifiers.size() - lowerQualifierParts.size();
                for (size_t i = 0; i < lowerQualifierParts.size(); ++i) {
                    if (toLowerCopy(ci.qualifiers[offset + i]) != lowerQualifierParts[i])
                        return false;
                }
                return true;
            }

            if (ci.kind != CompletionKind::Column || ci.matchText.empty())
                return false;

            const auto lastDot = ci.matchText.rfind('.');
            if (lastDot == std::string::npos)
                return false;

            std::vector<std::string> ownerParts;
            std::string currentPart;
            for (const char ch : ci.matchText.substr(0, lastDot)) {
                if (ch == '.') {
                    if (!currentPart.empty()) {
                        ownerParts.push_back(toLowerCopy(currentPart));
                        currentPart.clear();
                    }
                } else {
                    currentPart.push_back(ch);
                }
            }
            if (!currentPart.empty())
                ownerParts.push_back(toLowerCopy(currentPart));

            if (ownerParts.size() < lowerQualifierParts.size())
                return false;

            const size_t offset = ownerParts.size() - lowerQualifierParts.size();
            for (size_t i = 0; i < lowerQualifierParts.size(); ++i) {
                if (ownerParts[offset + i] != lowerQualifierParts[i])
                    return false;
            }
            return true;
        };

        struct ScoredItem {
            CompletionItem item;
            int score;
        };
        std::vector<ScoredItem> scored;

        for (const auto& item : items) {
            if (!qualifiersMatch(item))
                continue;
            if (!lowerQualifierParts.empty() && item.qualifiers.empty() &&
                item.kind != CompletionKind::Column)
                continue;

            const std::string insertText = item.insertText.empty() ? item.text : item.insertText;
            if (!lowerWord.empty() && toLowerCopy(insertText) == lowerWord)
                continue;

            const std::string matchText = item.matchText.empty() ? item.text : item.matchText;
            const std::string lowerMatchText = toLowerCopy(matchText);

            if (item.kind == CompletionKind::Column && !hasTableContext &&
                ctx != SqlCompletionContext::SelectWhereOn) {
                continue;
            }

            if (item.kind == CompletionKind::Column && ctx == SqlCompletionContext::SelectWhereOn &&
                !hasTableContext) {
                continue;
            }

            int score = 0;
            if (lowerWord.empty()) {
                score = 100;
            } else if (lowerMatchText.find(lowerWord) == 0) {
                score = 300;
            } else if (lowerMatchText.find(lowerWord) != std::string::npos) {
                score = 100;
            } else {
                continue;
            }

            switch (item.kind) {
            case CompletionKind::Column:
                score += 30;
                break;
            case CompletionKind::Table:
                score += 25;
                break;
            case CompletionKind::View:
                score += 20;
                break;
            case CompletionKind::Function:
                score += 15;
                break;
            case CompletionKind::Sequence:
                score += 10;
                break;
            case CompletionKind::Keyword:
                score -= 10;
                break;
            }

            if (ctx == SqlCompletionContext::FromJoin) {
                if (item.kind == CompletionKind::Table)
                    score += 80;
                else if (item.kind == CompletionKind::View)
                    score += 60;
                else if (item.kind == CompletionKind::Column)
                    continue;
                else if (item.kind == CompletionKind::Keyword)
                    score -= 40;
            } else if (ctx == SqlCompletionContext::SelectWhereOn) {
                if (item.kind == CompletionKind::Column)
                    score += 80;
                else if (item.kind == CompletionKind::Function)
                    score += 50;
                else if (item.kind == CompletionKind::Keyword)
                    score -= 20;
            }

            if (lowerMatchText.size() < 15)
                score += 5;

            if (item.kind == CompletionKind::Column && !referencedTables.empty()) {
                for (const auto& tableName : referencedTables) {
                    if (lowerMatchText.find(toLowerCopy(tableName)) != std::string::npos) {
                        score += 100;
                        break;
                    }
                }
            }

            scored.push_back({item, score});
        }

        std::sort(scored.begin(), scored.end(), [](const ScoredItem& a, const ScoredItem& b) {
            if (a.score != b.score)
                return a.score > b.score;
            if (a.item.text != b.item.text) {
                return std::lexicographical_compare(
                    a.item.text.begin(), a.item.text.end(), b.item.text.begin(), b.item.text.end(),
                    [](char x, char y) { return std::tolower(x) < std::tolower(y); });
            }
            return std::lexicographical_compare(
                a.item.detailText.begin(), a.item.detailText.end(), b.item.detailText.begin(),
                b.item.detailText.end(),
                [](char x, char y) { return std::tolower(x) < std::tolower(y); });
        });

        std::vector<CompletionItem> filtered;
        filtered.reserve(scored.size());
        for (auto& item : scored)
            filtered.push_back(std::move(item.item));
        return filtered;
    }

    std::string_view trimSqlView(const std::string& sql) {
        size_t start = 0;
        while (start < sql.size() && std::isspace(static_cast<unsigned char>(sql[start]))) {
            ++start;
        }

        size_t end = sql.size();
        while (end > start && std::isspace(static_cast<unsigned char>(sql[end - 1]))) {
            --end;
        }

        return std::string_view(sql).substr(start, end - start);
    }

    std::string getLeadingSqlKeyword(std::string_view sql) {
        size_t pos = 0;
        while (pos < sql.size() &&
               (std::isalpha(static_cast<unsigned char>(sql[pos])) || sql[pos] == '_')) {
            ++pos;
        }
        return toLowerCopy(sql.substr(0, pos));
    }
} // namespace

SQLEditorTab::SQLEditorTab(const std::string& name, IDatabaseNode* node,
                           const std::string& schemaName)
    : Tab(name, TabType::SQL_EDITOR), node_(node), selectedSchemaName(schemaName),
      scriptName_(name) {
    sqlEditor.SetShowLineNumbers(true);
    sqlEditor.SetSubmitCallback([this] {
        if (sqlEditor.HasSelection()) {
            startQueryExecutionAsync(sqlEditor.GetSelectedText());
        } else {
            sqlQuery = sqlEditor.GetText();
            startQueryExecutionAsync(sqlQuery);
        }
    });
    sqlEditor.SetCompletionFilter(filterSqlCompletions);
    bindNode(node_);
    scheduleSyntaxCheck();
    // seed rename buffer with initial name
    std::strncpy(renameBuffer_, scriptName_.c_str(), sizeof(renameBuffer_) - 1);
}

SQLEditorTab::~SQLEditorTab() {
    queryExecutionOp_.cancel();
}

void SQLEditorTab::render() {
    // Sync editor palette with current app theme
    const bool dark = Application::getInstance().isDarkTheme();
    sqlEditor.SetPalette(
        dearsql::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));
    syncBoundNodePointer();

    if (!completionKeywordsSet_) {
        updateCompletionKeywords();
    }

    checkQueryExecutionStatus();
    updateSyntaxDiagnostics();

    // Cmd+S / Ctrl+S save shortcut — flag here, execute after editor text is synced below
    const bool wantSave = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                          (ImGui::GetIO().KeyMods & ImGuiMod_Shortcut) &&
                          ImGui::IsKeyPressed(ImGuiKey_S, false);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);
    renderConnectionInfo();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);
    renderScriptHeader();

    // Render AI settings dialog (modal, always available)
    AISettingsDialog::instance().render();

    constexpr float toggleStripWidth = 28.0f;
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    totalContentHeight = ImGui::GetContentRegionAvail().y;

    const float panelContentWidth = aiPanelVisible_ ? aiPanelWidth_ : 0.0f;
    float editorAreaWidth = totalWidth - toggleStripWidth - panelContentWidth;
    editorAreaWidth = std::max(200.0f, editorAreaWidth);

    // Left pane: editor + results
    if (ImGui::BeginChild("##sql_left_pane", ImVec2(editorAreaWidth, totalContentHeight), false)) {
        float paneHeight = ImGui::GetContentRegionAvail().y;
        const float toolbarHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::S;
        const float editorHeight = paneHeight * splitterPosition;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition) - 6.0f - toolbarHeight;

        if (ImGui::BeginChild("SQLEditor", ImVec2(-1, editorHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            if (pendingEditorFocusFrames_ > 0 && !renamingScript_) {
                sqlEditor.SetFocus();
                pendingEditorFocusFrames_--;
            }
            sqlEditor.Render("##SQL", ImVec2(-1, -1), true);
            const std::string newText = sqlEditor.GetText();
            if (newText != sqlQuery) {
                sqlQuery = newText;
                contentModified_ = true;
                scheduleSyntaxCheck();
            }
        }
        ImGui::EndChild();

        // execute save now that sqlQuery is guaranteed up-to-date
        if (wantSave) {
            saveScript();
        }

        renderToolbar();
        UIUtils::Splitter("##sql_splitter", &splitterPosition, totalContentHeight, 100.0f, 200.0f);

        if (ImGui::BeginChild("SQLResults", ImVec2(-1, resultsHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            ImVec2 contentStart = ImGui::GetCursorScreenPos();
            const bool isRunning = queryExecutionOp_.isRunning();
            if (isRunning)
                ImGui::BeginDisabled();
            renderQueryResults();
            if (isRunning)
                ImGui::EndDisabled();

            // Spinner overlay while executing
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
                UIUtils::Spinner("##results_spinner", spinnerRadius, 2,
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
        renderAIPanel(panelContentWidth, totalContentHeight);
    }

    // Toggle strip on the far right (always visible)
    ImGui::SameLine(0, 0);
    renderAIToggleStrip(toggleStripWidth, totalContentHeight);
}

void SQLEditorTab::renderConnectionInfo() {
    if (!node_) {
        ImGui::Text("%s", LABEL_NO_DATABASE);
        ImGui::Separator();
        return;
    }

    switch (node_->getDatabaseType()) {
    case DatabaseType::REDSHIFT:
    case DatabaseType::POSTGRESQL:
        renderConnectionInfoPostgres();
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        renderConnectionInfoMySQL();
        break;
    case DatabaseType::MSSQL:
        renderConnectionInfoMSSQL();
        break;
    case DatabaseType::ORACLE:
        renderConnectionInfoOracle();
        break;
    case DatabaseType::SQLITE:
        renderConnectionInfoSQLite();
        break;
    default:
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
        break;
    }

    ImGui::Separator();
}

void SQLEditorTab::renderConnectionInfoPostgres() {
    // Database-level editor: PostgresDatabaseNode bound directly
    if (auto* pgDbNode = dynamic_cast<PostgresDatabaseNode*>(node_)) {
        auto* serverDb = pgDbNode->parentDb;
        if (!serverDb) {
            ImGui::Text("Database: %s", node_->getFullPath().c_str());
            return;
        }

        const auto& dbMap = serverDb->getDatabaseDataMap();
        std::vector<std::string> dbNames;
        dbNames.reserve(dbMap.size());
        for (const auto& dbName : dbMap | std::views::keys)
            dbNames.push_back(dbName);
        std::ranges::sort(dbNames);

        renderDatabaseCombo(serverDb->getConnectionInfo().host, "Database:", pgDbNode->name,
                            dbNames, [serverDb, this](const std::string& selectedName) {
                                if (auto* n = serverDb->getDatabaseData(selectedName))
                                    switchNode(n);
                            });
        return;
    }

    // Schema-level editor: PostgresSchemaNode bound (backward compat)
    auto* dbNode = dynamic_cast<PostgresDatabaseNode*>(node_);
    auto* schemaNode = dynamic_cast<PostgresSchemaNode*>(node_);
    if (!dbNode && schemaNode)
        dbNode = schemaNode->parentDbNode;

    if (!dbNode || !dbNode->parentDb) {
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
        return;
    }

    auto* serverDb = dbNode->parentDb;
    const auto& connInfo = serverDb->getConnectionInfo();
    const auto& colors = Application::getInstance().getCurrentColors();

    const auto& dbMap = serverDb->getDatabaseDataMap();
    std::vector<std::string> dbNames;
    dbNames.reserve(dbMap.size());
    for (const auto& name : dbMap | std::views::keys) {
        dbNames.push_back(name);
    }
    std::ranges::sort(dbNames);

    if (!schemaNode) {
        renderDatabaseCombo(connInfo.host, "Database:", dbNode->name, dbNames,
                            [this, serverDb](const std::string& selectedDb) {
                                if (auto* targetDb = serverDb->getDatabaseData(selectedDb))
                                    switchNode(targetDb);
                            });
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", connInfo.host.c_str());
    ImGui::SameLine(0, Theme::Spacing::L);

    // Single "Schema" combo: database names as headers, schemas as selectable items
    std::string preview = std::format("{}.{}", dbNode->name, schemaNode->name);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Schema:");
    ImGui::SameLine(0, Theme::Spacing::S);

    // Handle pending database switch (schemas were loading when user selected)
    if (!pendingDatabaseSwitch_.empty()) {
        auto* pendingDb = serverDb->getDatabaseData(pendingDatabaseSwitch_);
        if (pendingDb) {
            pendingDb->checkSchemasStatusAsync();
            if (pendingDb->schemasLoaded && !pendingDb->schemas.empty()) {
                switchNode(pendingDb->schemas[0].get());
                pendingDatabaseSwitch_.clear();
                schemaNode = dynamic_cast<PostgresSchemaNode*>(node_);
                if (!schemaNode || !schemaNode->parentDbNode)
                    return;
                dbNode = schemaNode->parentDbNode;
            }
        } else {
            pendingDatabaseSwitch_.clear();
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::S, Theme::Spacing::S));
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);

    if (queryExecutionOp_.isRunning())
        ImGui::BeginDisabled();

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##schema_combo", preview.c_str())) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, Theme::Spacing::XS));
        bool first = true;
        for (const auto& dbName : dbNames) {
            auto* db = serverDb->getDatabaseData(dbName);
            if (!db)
                continue;

            // Ensure schemas are loaded
            if (!db->schemasLoaded && !db->schemasLoader.isRunning()) {
                db->startSchemasLoadAsync();
            }
            db->checkSchemasStatusAsync();

            if (!first) {
                ImGui::Separator();
            }
            first = false;

            // Database name as non-selectable header
            ImGui::TextDisabled("%s", dbName.c_str());

            if (!db->schemasLoaded) {
                ImGui::Indent(Theme::Spacing::L);
                ImGui::TextDisabled("Loading...");
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner(std::format("##loading_schemas_{}", dbName).c_str(), 5.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_TextDisabled));
                ImGui::Unindent(Theme::Spacing::L);
            } else {
                for (const auto& schema : db->schemas) {
                    bool isSelected = (schema.get() == node_);
                    std::string label =
                        std::format("  {}##{}.{}", schema->name, dbName, schema->name);
                    if (ImGui::Selectable(
                            label.c_str(), isSelected, ImGuiSelectableFlags_None,
                            ImVec2(0, ImGui::GetTextLineHeight() + Theme::Spacing::S))) {
                        if (schema.get() != node_) {
                            switchNode(schema.get());
                        }
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (queryExecutionOp_.isRunning())
        ImGui::EndDisabled();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void SQLEditorTab::renderConnectionInfoMySQL() {
    auto* dbNode = dynamic_cast<MySQLDatabaseNode*>(node_);
    if (!dbNode || !dbNode->parentDb) {
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
        return;
    }

    auto* serverDb = dbNode->parentDb;
    const auto& dbMap = serverDb->getDatabaseDataMap();
    std::vector<std::string> dbNames;
    dbNames.reserve(dbMap.size());
    for (const auto& name : dbMap | std::views::keys)
        dbNames.push_back(name);
    std::ranges::sort(dbNames);

    renderDatabaseCombo(serverDb->getConnectionInfo().host, "Database:", dbNode->name, dbNames,
                        [serverDb, this](const std::string& name) {
                            if (auto* n = serverDb->getDatabaseData(name))
                                switchNode(n);
                        });
}

void SQLEditorTab::renderConnectionInfoMSSQL() {
    MSSQLDatabaseNode* dbNode = nullptr;
    MSSQLSchemaNode* schemaNode = nullptr;

    if (auto* sn = dynamic_cast<MSSQLSchemaNode*>(node_)) {
        schemaNode = sn;
        dbNode = sn->parentDbNode;
    } else {
        dbNode = dynamic_cast<MSSQLDatabaseNode*>(node_);
    }

    if (!dbNode || !dbNode->parentDb) {
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
        return;
    }

    auto* serverDb = dbNode->parentDb;
    const auto& dbMap = serverDb->getDatabaseDataMap();
    std::vector<std::string> dbNames;
    dbNames.reserve(dbMap.size());
    for (const auto& name : dbMap | std::views::keys)
        dbNames.push_back(name);
    std::ranges::sort(dbNames);

    renderDatabaseCombo(serverDb->getConnectionInfo().host, "Database:", dbNode->name, dbNames,
                        [serverDb, this](const std::string& name) {
                            if (auto* n = serverDb->getDatabaseData(name)) {
                                // switch to first schema if available
                                if (!n->schemas.empty())
                                    switchNode(n->schemas.front().get());
                                else
                                    switchNode(n);
                            }
                        });

    // schema combo
    if (dbNode->schemasLoaded && !dbNode->schemas.empty() && schemaNode) {
        std::vector<std::string> schemaNames;
        schemaNames.reserve(dbNode->schemas.size());
        for (const auto& s : dbNode->schemas)
            if (s)
                schemaNames.push_back(s->name);

        ImGui::SameLine(0, Theme::Spacing::L);
        renderDatabaseCombo("", "Schema:", schemaNode->name, schemaNames,
                            [dbNode, this](const std::string& name) {
                                for (const auto& s : dbNode->schemas) {
                                    if (s && s->name == name) {
                                        switchNode(s.get());
                                        return;
                                    }
                                }
                            });
    }
}

void SQLEditorTab::renderConnectionInfoOracle() {
    auto* dbNode = dynamic_cast<OracleDatabaseNode*>(node_);
    if (!dbNode || !dbNode->parentDb) {
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
        return;
    }

    auto* serverDb = dbNode->parentDb;
    const auto& dbMap = serverDb->getDatabaseDataMap();
    std::vector<std::string> dbNames;
    dbNames.reserve(dbMap.size());
    for (const auto& name : dbMap | std::views::keys)
        dbNames.push_back(name);
    std::ranges::sort(dbNames);

    renderDatabaseCombo(serverDb->getConnectionInfo().host, "Schema:", dbNode->name, dbNames,
                        [serverDb, this](const std::string& name) {
                            if (auto* n = serverDb->getDatabaseData(name))
                                switchNode(n);
                        });
}

void SQLEditorTab::renderConnectionInfoSQLite() {
    ImGui::Text("Database: %s", node_->getFullPath().c_str());
}

void SQLEditorTab::renderDatabaseCombo(const std::string& host, const char* label,
                                       const std::string& currentName,
                                       const std::vector<std::string>& dbNames,
                                       const std::function<void(const std::string&)>& onSelect) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", host.c_str());
    ImGui::SameLine(0, Theme::Spacing::L);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine(0, Theme::Spacing::S);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::S, Theme::Spacing::S));
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);

    if (queryExecutionOp_.isRunning())
        ImGui::BeginDisabled();

    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##db_combo", currentName.c_str())) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, Theme::Spacing::XS));
        for (const auto& name : dbNames) {
            bool isSelected = (name == currentName);
            if (ImGui::Selectable(name.c_str(), isSelected, ImGuiSelectableFlags_None,
                                  ImVec2(0, ImGui::GetTextLineHeight() + Theme::Spacing::S))) {
                if (name != currentName) {
                    onSelect(name);
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (queryExecutionOp_.isRunning())
        ImGui::EndDisabled();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void SQLEditorTab::switchNode(IDatabaseNode* newNode) {
    if (!newNode || newNode == node_)
        return;

    node_ = newNode;
    bindNode(node_);
    completionKeywordsSet_ = false;

    if (aiChatState_) {
        aiChatState_->setDatabaseNode(node_);
    }
}

void SQLEditorTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);

    if (queryExecutionOp_.isRunning()) {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_PLAY " Run");
        ImGui::EndDisabled();

        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(LABEL_CANCEL)) {
            cancelQueryExecution();
        }
    } else {
        if (ImGui::Button(ICON_FA_PLAY " Run")) {
            if (sqlEditor.HasSelection()) {
                startQueryExecutionAsync(sqlEditor.GetSelectedText());
            } else {
                startQueryExecutionAsync(sqlQuery);
            }
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(ICON_FA_ALIGN_LEFT " Format")) {
            formatSQL();
        }
    }

    if (syntaxDiagnostic_.active) {
        ImGui::SameLine(0, Theme::Spacing::L);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        const std::string inlineMessage =
            std::string(ICON_FA_TRIANGLE_EXCLAMATION) + " " + syntaxDiagnostic_.message;
        ImGui::TextUnformatted(inlineMessage.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void SQLEditorTab::renderQueryResults() const {
    if (queryResult.empty()) {
        ImGui::Text("%s", LABEL_NO_RESULTS);
        return;
    }

    // Show execution time above results
    if (queryResult.executionTimeMs > 0) {
        ImGui::Text("Execution time: %.2f ms", queryResult.executionTimeMs);
    }

    // Single result — render directly without tabs
    if (queryResult.size() == 1) {
        renderSingleResult(queryResult[0], 0);
        return;
    }

    // Multiple results — render as tabs
    if (ImGui::BeginTabBar("##QueryResultTabs")) {
        int tabIndex = 0;
        for (size_t i = 0; i < queryResult.size(); ++i) {
            const auto& r = queryResult[i];

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

void SQLEditorTab::renderSingleResult(const StatementResult& r, size_t index) const {
    if (!r.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", r.errorMessage.c_str());
        return;
    }

    if (r.columnNames.empty()) {
        // DML/DDL result
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", r.message.c_str());
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

    if (!r.tableData.empty()) {
        float tableHeight = std::max(ImGui::GetContentRegionAvail().y - 20.0f, 50.0f);

        TableRenderer::Config config;
        config.allowEditing = false;
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
    if (queryExecutionOp_.isRunning()) {
        return;
    }

    queryError.clear();
    lastQueryDuration = std::chrono::milliseconds{0};

    syncBoundNodePointer();

    IQueryExecutor* executor = nullptr;
    if (binding_.resolveExecutor) {
        executor = binding_.resolveExecutor();
    }

    if (executor) {
        queryExecutionOp_.startCancellable([query, executor](const std::stop_token& stopToken) {
            QueryResult result;

            if (stopToken.stop_requested()) {
                return result;
            }

            result = executor->executeQuery(query);

            if (stopToken.stop_requested()) {
                return QueryResult{};
            }
            return result;
        });
    }
    StatementResult r;
    r.success = false;
    r.errorMessage = LABEL_NO_DATABASE_SELECTED;
    queryResult = QueryResult{};
    queryResult.statements.push_back(r);
}

void SQLEditorTab::bindNode(IDatabaseNode* node) {
    binding_ = {};
    if (!node) {
        return;
    }

    // PostgresDatabaseNode: database-level editor (no SET search_path — cross-schema queries)
    if (auto* dbNode = dynamic_cast<PostgresDatabaseNode*>(node); dbNode && dbNode->parentDb) {
        const std::string dbName = dbNode->name;
        binding_.resolveNode = [serverDb = dbNode->parentDb, dbName]() -> IDatabaseNode* {
            if (auto* resolved = const_cast<PostgresDatabaseNode*>(
                    static_cast<const PostgresDatabase*>(serverDb)->getDatabaseData(dbName))) {
                return resolved;
            }

            if (!serverDb->areDatabasesLoaded() && !serverDb->isLoadingDatabases()) {
                serverDb->refreshDatabaseNames();
            }
            serverDb->checkDatabasesStatusAsync();
            return const_cast<PostgresDatabaseNode*>(
                static_cast<const PostgresDatabase*>(serverDb)->getDatabaseData(dbName));
        };
        binding_.resolveExecutor = [this]() -> IQueryExecutor* {
            return binding_.resolveNode ? binding_.resolveNode() : nullptr;
        };
        return;
    }

    if (const auto* schemaNode = dynamic_cast<PostgresSchemaNode*>(node);
        schemaNode && schemaNode->parentDbNode && schemaNode->parentDbNode->parentDb) {
        const std::string dbName = schemaNode->parentDbNode->name;
        const std::string schemaName = schemaNode->name;

        // Use the schema node as executor so queries go through PostgresSchemaNode::executeQuery()
        // and apply the correct search_path, while still re-resolving by name after refreshes.
        binding_.resolveNode = [schemaNode, dbName, schemaName]() -> IDatabaseNode* {
            auto* serverDb = schemaNode->parentDbNode->parentDb;
            auto* dbNode = serverDb->getDatabaseData(dbName);
            if (!dbNode) {
                return nullptr;
            }

            auto resolveByName = [&]() -> PostgresSchemaNode* {
                for (const auto& schema : dbNode->schemas) {
                    if (schema && schema->name == schemaName) {
                        return schema.get();
                    }
                }
                return nullptr;
            };

            if (auto* schema = resolveByName()) {
                return schema;
            }

            if (!dbNode->schemasLoaded && !dbNode->schemasLoader.isRunning()) {
                dbNode->startSchemasLoadAsync();
            }
            dbNode->checkSchemasStatusAsync();
            if (auto* schema = resolveByName()) {
                return schema;
            }

            if (!dbNode->schemas.empty() && dbNode->schemas.front()) {
                return dbNode->schemas.front().get();
            }

            for (const auto& candidateDb : serverDb->getDatabaseDataMap() | std::views::values) {
                if (candidateDb && !candidateDb->schemas.empty() && candidateDb->schemas.front()) {
                    return candidateDb->schemas.front().get();
                }
            }

            return nullptr;
        };
        binding_.resolveExecutor = [this]() -> IQueryExecutor* {
            return binding_.resolveNode ? binding_.resolveNode() : nullptr;
        };
        return;
    }

    binding_.resolveNode = [node]() -> IDatabaseNode* { return node; };
    binding_.resolveExecutor = [node]() -> IQueryExecutor* { return node; };
}

void SQLEditorTab::syncBoundNodePointer() {
    if (!binding_.resolveNode) {
        return;
    }

    auto* resolved = binding_.resolveNode();
    if (resolved == node_) {
        return;
    }

    node_ = resolved;
    completionKeywordsSet_ = false;
    if (aiChatState_) {
        aiChatState_->setDatabaseNode(node_);
    }
}

void SQLEditorTab::checkQueryExecutionStatus() {
    try {
        queryExecutionOp_.check([this](QueryResult result) {
            if (!result.empty() && !result.success()) {
                queryError = result.errorMessage();
                SentryUtils::addBreadcrumb("query", "Query error", "error", queryError, "error");
            }

            lastQueryDuration =
                std::chrono::milliseconds{static_cast<long long>(result.executionTimeMs)};
            queryResult = std::move(result);
        });
    } catch (const std::exception& e) {
        queryError = "Error in async query execution: " + std::string(e.what());
    }
}

void SQLEditorTab::cancelQueryExecution() {
    queryExecutionOp_.cancel();
}

void SQLEditorTab::formatSQL() {
    std::string formatted = dearsql::TextEditor::FormatSQL(sqlEditor.GetText());
    if (!formatted.empty()) {
        sqlEditor.SetText(formatted);
        sqlQuery = formatted;
        scheduleSyntaxCheck();
    }
}

void SQLEditorTab::scheduleSyntaxCheck() {
    syntaxCheckPending_ = true;
    syntaxCheckDelay_ = 0.25f;
}

void SQLEditorTab::updateSyntaxDiagnostics() {
    if (!syntaxCheckPending_) {
        return;
    }

    syntaxCheckDelay_ -= ImGui::GetIO().DeltaTime;
    if (syntaxCheckDelay_ > 0.0f) {
        return;
    }

    syntaxCheckPending_ = false;
    syntaxDiagnostic_ = {};

    const std::string_view trimmed = trimSqlView(sqlQuery);
    if (trimmed.empty()) {
        return;
    }

    hsql::SQLParserResult parseResult;
    if (!hsql::SQLParser::parse(sqlQuery, &parseResult)) {
        syntaxDiagnostic_.active = true;
        syntaxDiagnostic_.message = "SQL parser failed internally while checking syntax.";
        return;
    }

    if (parseResult.isValid()) {
        return;
    }

    syntaxDiagnostic_.active = true;
    syntaxDiagnostic_.line = std::max(1, parseResult.errorLine());
    syntaxDiagnostic_.column = std::max(1, parseResult.errorColumn());

    const std::string keyword = getLeadingSqlKeyword(trimmed);
    if (keyword == "alter" || keyword == "explain" || keyword == "rename" || keyword == "export") {
        syntaxDiagnostic_.message =
            std::format("The embedded parser does not fully support '{}' statements yet.", keyword);
        return;
    }

    const char* parserMessage = parseResult.errorMsg();
    if (parserMessage && parserMessage[0] != '\0') {
        syntaxDiagnostic_.message = parserMessage;
    } else {
        syntaxDiagnostic_.message = "Syntax issue.";
    }
}

void SQLEditorTab::updateCompletionKeywords() {
    std::vector<CompletionItem> items;

    // SQL keywords
    std::set<std::string> seenLabels;
    for (const auto& kw : dearsql::TextEditor::GetDefaultCompletionKeywords()) {
        items.push_back({kw, CompletionKind::Keyword});
        seenLabels.insert(kw);
    }

    // SQL functions (aggregate, string, date, math, conditional)
    static const std::vector<std::string> sqlFunctions = {
        // Aggregate
        "COUNT",
        "SUM",
        "AVG",
        "MIN",
        "MAX",
        "GROUP_CONCAT",
        "STRING_AGG",
        "ARRAY_AGG",
        // String
        "CONCAT",
        "LENGTH",
        "UPPER",
        "LOWER",
        "TRIM",
        "LTRIM",
        "RTRIM",
        "SUBSTRING",
        "REPLACE",
        "LEFT",
        "RIGHT",
        "REVERSE",
        "REPEAT",
        "POSITION",
        "STRPOS",
        "SPLIT_PART",
        "INITCAP",
        "LPAD",
        "RPAD",
        "TRANSLATE",
        "FORMAT",
        // Date/Time
        "NOW",
        "CURRENT_DATE",
        "CURRENT_TIME",
        "CURRENT_TIMESTAMP",
        "DATE_TRUNC",
        "DATE_PART",
        "EXTRACT",
        "AGE",
        "DATE_ADD",
        "DATE_SUB",
        "DATEDIFF",
        "DATEADD",
        "GETDATE",
        "SYSDATE",
        "TO_DATE",
        "TO_CHAR",
        "TO_TIMESTAMP",
        "INTERVAL",
        // Math
        "ABS",
        "CEIL",
        "CEILING",
        "FLOOR",
        "ROUND",
        "MOD",
        "POWER",
        "SQRT",
        "SIGN",
        "RANDOM",
        "LOG",
        "LN",
        "EXP",
        "PI",
        "GREATEST",
        "LEAST",
        // Conditional
        "COALESCE",
        "NULLIF",
        "IFNULL",
        "NVL",
        "NVL2",
        "DECODE",
        "IIF",
        // Type conversion
        "CAST",
        "CONVERT",
        "TRY_CAST",
        // Window
        "ROW_NUMBER",
        "RANK",
        "DENSE_RANK",
        "NTILE",
        "LAG",
        "LEAD",
        "FIRST_VALUE",
        "LAST_VALUE",
        "NTH_VALUE",
        // JSON (PostgreSQL/MySQL)
        "JSON_AGG",
        "JSON_BUILD_OBJECT",
        "JSON_EXTRACT",
        "JSON_EXTRACT_PATH",
        "JSONB_BUILD_OBJECT",
        "JSON_OBJECT",
        "JSON_ARRAY",
        // Other
        "EXISTS",
        "IN",
        "ANY",
        "ALL",
        "SOME",
        "GENERATE_SERIES",
        "UNNEST",
        "ARRAY_LENGTH",
    };
    for (const auto& fn : sqlFunctions) {
        if (seenLabels.insert(fn).second)
            items.push_back({fn, CompletionKind::Function});
    }

    auto addColumnsFromNode = [&](IDatabaseNode* sourceNode,
                                  const std::vector<std::string>& qualifiers = {}) {
        if (!sourceNode)
            return;
        for (const auto& table : sourceNode->getTables()) {
            for (const auto& col : table.columns) {
                CompletionItem colItem(col.name, CompletionKind::Column);
                std::string owner = joinQualifiers(qualifiers);
                if (!owner.empty())
                    owner += ".";
                owner += table.name;

                colItem.matchText = owner.empty() ? col.name : owner + "." + col.name;

                std::string detail = table.name;
                if (!col.type.empty()) {
                    if (!detail.empty())
                        detail += "  ";
                    detail += col.type;
                }
                colItem.detailText = detail;
                items.push_back(std::move(colItem));
            }
        }
    };

    auto addNodeObjects = [&](IDatabaseNode* sourceNode,
                              const std::vector<std::string>& qualifiers) {
        if (!sourceNode)
            return;

        for (const auto& table : sourceNode->getTables())
            items.push_back(makeCompletionItem(table.name, CompletionKind::Table, qualifiers));

        for (const auto& view : sourceNode->getViews())
            items.push_back(makeCompletionItem(view.name, CompletionKind::View, qualifiers));

        for (const auto& seq : sourceNode->getSequences())
            items.push_back(makeCompletionItem(seq, CompletionKind::Sequence, qualifiers));
    };

    auto finalizePartialItems = [&]() {
        sortAndDeduplicateCompletionItems(items);
        sqlEditor.SetCompletionItems(items);
    };

    if (auto* dbNode = dynamic_cast<PostgresDatabaseNode*>(node_); dbNode) {
        dbNode->checkSchemasStatusAsync();
        if (!dbNode->schemasLoaded && !dbNode->schemasLoader.isRunning())
            dbNode->startSchemasLoadAsync();

        bool tablesLoaded = dbNode->schemasLoaded;
        bool viewsLoaded = dbNode->schemasLoaded;
        for (const auto& schema : dbNode->schemas) {
            if (!schema)
                continue;
            scheduleMetadataLoad(schema.get());
            tablesLoaded = tablesLoaded && schema->isTablesLoaded();
            viewsLoaded = viewsLoaded && schema->isViewsLoaded();
            addNodeObjects(schema.get(), {schema->name});
            addColumnsFromNode(schema.get(), {dbNode->name, schema->name});
        }

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    } else if (auto* schemaNode = dynamic_cast<PostgresSchemaNode*>(node_);
               schemaNode && schemaNode->parentDbNode) {
        auto* dbNode = schemaNode->parentDbNode;
        dbNode->checkSchemasStatusAsync();
        if (!dbNode->schemasLoaded && !dbNode->schemasLoader.isRunning())
            dbNode->startSchemasLoadAsync();

        bool tablesLoaded = true;
        bool viewsLoaded = true;
        for (const auto& schema : dbNode->schemas) {
            if (!schema)
                continue;
            scheduleMetadataLoad(schema.get());
            tablesLoaded = tablesLoaded && schema->isTablesLoaded();
            viewsLoaded = viewsLoaded && schema->isViewsLoaded();
            addNodeObjects(schema.get(), {schema->name});
            if (schema.get() == schemaNode)
                addColumnsFromNode(schema.get(), {dbNode->name, schema->name});
        }

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    } else if (auto* mySqlNode = dynamic_cast<MySQLDatabaseNode*>(node_);
               mySqlNode && mySqlNode->parentDb) {
        auto* serverDb = mySqlNode->parentDb;
        serverDb->checkDatabasesStatusAsync();

        bool tablesLoaded = true;
        bool viewsLoaded = true;
        for (const auto& dbEntry : serverDb->getDatabaseDataMap() | std::views::values) {
            if (!dbEntry)
                continue;
            scheduleMetadataLoad(dbEntry.get());
            tablesLoaded = tablesLoaded && dbEntry->isTablesLoaded();
            viewsLoaded = viewsLoaded && dbEntry->isViewsLoaded();
            addNodeObjects(dbEntry.get(), {dbEntry->name});
            if (dbEntry.get() == mySqlNode)
                addColumnsFromNode(dbEntry.get(), {dbEntry->name});
        }

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    } else if (auto* msSqlSchemaNode = dynamic_cast<MSSQLSchemaNode*>(node_);
               msSqlSchemaNode && msSqlSchemaNode->parentDbNode) {
        auto* msSqlDbNode = msSqlSchemaNode->parentDbNode;
        auto* serverDb = msSqlDbNode->parentDb;
        if (serverDb)
            serverDb->checkDatabasesStatusAsync();

        bool tablesLoaded = true;
        bool viewsLoaded = true;
        for (const auto& dbEntry : serverDb->getDatabaseDataMap() | std::views::values) {
            if (!dbEntry)
                continue;

            dbEntry->checkSchemasStatusAsync();
            if (!dbEntry->schemasLoaded) {
                if (!dbEntry->schemasLoader.isRunning())
                    dbEntry->startSchemasLoadAsync();
                finalizePartialItems();
                return;
            }

            for (const auto& schema : dbEntry->schemas) {
                if (!schema)
                    continue;
                scheduleMetadataLoad(schema.get());
                tablesLoaded = tablesLoaded && schema->isTablesLoaded();
                viewsLoaded = viewsLoaded && schema->isViewsLoaded();
                addNodeObjects(schema.get(), {dbEntry->name, schema->name});
                if (schema.get() == msSqlSchemaNode)
                    addColumnsFromNode(schema.get(), {dbEntry->name, schema->name});
            }
        }

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    } else if (auto* msSqlNode = dynamic_cast<MSSQLDatabaseNode*>(node_);
               msSqlNode && msSqlNode->parentDb) {
        auto* serverDb = msSqlNode->parentDb;
        serverDb->checkDatabasesStatusAsync();

        bool tablesLoaded = true;
        bool viewsLoaded = true;
        for (const auto& dbEntry : serverDb->getDatabaseDataMap() | std::views::values) {
            if (!dbEntry)
                continue;

            dbEntry->checkSchemasStatusAsync();
            if (!dbEntry->schemasLoaded) {
                if (!dbEntry->schemasLoader.isRunning())
                    dbEntry->startSchemasLoadAsync();
                finalizePartialItems();
                return;
            }

            for (const auto& schema : dbEntry->schemas) {
                if (!schema)
                    continue;
                scheduleMetadataLoad(schema.get());
                tablesLoaded = tablesLoaded && schema->isTablesLoaded();
                viewsLoaded = viewsLoaded && schema->isViewsLoaded();
                addNodeObjects(schema.get(), {dbEntry->name, schema->name});
            }
            if (dbEntry.get() == msSqlNode && !dbEntry->schemas.empty())
                addColumnsFromNode(dbEntry->schemas.front().get(),
                                   {dbEntry->name, dbEntry->schemas.front()->name});
        }

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    } else if (auto* oracleNode = dynamic_cast<OracleDatabaseNode*>(node_);
               oracleNode && oracleNode->parentDb) {
        auto* serverDb = oracleNode->parentDb;
        serverDb->checkDatabasesStatusAsync();

        bool tablesLoaded = true;
        bool viewsLoaded = true;
        for (const auto& schemaEntry : serverDb->getDatabaseDataMap() | std::views::values) {
            if (!schemaEntry)
                continue;
            scheduleMetadataLoad(schemaEntry.get());
            tablesLoaded = tablesLoaded && schemaEntry->isTablesLoaded();
            viewsLoaded = viewsLoaded && schemaEntry->isViewsLoaded();
            addNodeObjects(schemaEntry.get(), {schemaEntry->name});
            if (schemaEntry.get() == oracleNode)
                addColumnsFromNode(schemaEntry.get(), {schemaEntry->name});
        }

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    } else {
        scheduleMetadataLoad(node_);
        const bool tablesLoaded = node_->isTablesLoaded();
        const bool viewsLoaded = node_->isViewsLoaded();

        addNodeObjects(node_, {});
        addColumnsFromNode(node_);

        if (!tablesLoaded || !viewsLoaded) {
            finalizePartialItems();
            return;
        }
    }

    // Sort and deduplicate by text
    sortAndDeduplicateCompletionItems(items);
    sqlEditor.SetCompletionItems(items);
    completionKeywordsSet_ = true;
}

void SQLEditorTab::initAIPanel() {
    aiChatState_ = std::make_unique<AIChatState>(node_);
    aiChatPanel_ = std::make_unique<AIChatPanel>(aiChatState_.get());
    aiChatPanel_->setInsertCallback([this](const std::string& sql) {
        std::string current = sqlEditor.GetText();
        if (!current.empty() && current.back() != '\n') {
            current += "\n";
        }
        current += sql;
        sqlEditor.SetText(current);
        sqlQuery = current;
        scheduleSyntaxCheck();
    });
}

void SQLEditorTab::renderAIToggleStrip(float stripWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("AIToggleStrip", ImVec2(stripWidth, availableHeight),
                          ImGuiChildFlags_None)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        // Draw left borderline
        drawList->AddLine(stripPos, ImVec2(stripPos.x, stripPos.y + availableHeight),
                          ImGui::GetColorU32(colors.overlay0), 1.0f);

        // Rotated "AI" label as a clickable tab
        const char* label = "Assistant";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        constexpr float padding = 6.0f;
        const float buttonW = stripWidth;
        const float buttonH = textSize.x + padding * 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y));
        ImGui::InvisibleButton("##toggleAI", ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            aiPanelVisible_ = !aiPanelVisible_;
            if (aiPanelVisible_ && !aiChatPanel_) {
                initAIPanel();
            }
        }

        // Button background
        const ImVec2 btnMin = stripPos;
        const ImVec2 btnMax(stripPos.x + buttonW, stripPos.y + buttonH);
        if (aiPanelVisible_) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        } else if (hovered) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        }

        // Bottom border of button area
        drawList->AddLine(ImVec2(btnMin.x, btnMax.y), btnMax, ImGui::GetColorU32(colors.overlay0),
                          1.0f);

        // Draw rotated text centered in the button area
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

        // Rotate all text vertices 90 degrees (top-to-bottom reading) around center
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

// ── Script file management ────────────────────────────────────────────────────

std::string SQLEditorTab::getDefaultScriptsDir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    const std::filesystem::path dir = home ? std::filesystem::path(home) / ".dearsql" / "scripts"
                                           : std::filesystem::path(".") / "scripts";
    std::filesystem::create_directories(dir);
    return dir.string();
}

void SQLEditorTab::saveScript() {
    // build file path if not yet set
    if (filePath_.empty()) {
        const std::string dir = getDefaultScriptsDir();
        // sanitize name for filesystem
        std::string safeName = scriptName_;
        for (char& c : safeName) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|')
                c = '_';
        }
        if (safeName.empty())
            safeName = "untitled";
        std::filesystem::path candidate = std::filesystem::path(dir) / (safeName + ".sql");
        // avoid collision with existing files from other scripts
        int n = 1;
        while (std::filesystem::exists(candidate) && scriptId_ == 0) {
            candidate =
                std::filesystem::path(dir) / (safeName + "_" + std::to_string(n++) + ".sql");
        }
        filePath_ = candidate.string();
    }

    // write content to disk
    std::ofstream out(filePath_, std::ios::out | std::ios::trunc);
    if (!out) {
        spdlog::error("Failed to write script file: {}", filePath_);
        return;
    }
    out << sqlQuery;
    out.close();

    contentModified_ = false;
    persistScriptToAppState();

    // sync tab display name
    setName(scriptName_);
    spdlog::debug("Saved script '{}' to {}", scriptName_, filePath_);
}

void SQLEditorTab::persistScriptToAppState() {
    auto* appState = Application::getInstance().getAppState();
    if (!appState)
        return;

    SqlScript s;
    s.id = scriptId_;
    s.name = scriptName_;
    s.filePath = filePath_;

    // resolve connection/database metadata from the current node
    if (node_) {
        if (auto* ownerDb = node_->ownerDatabase()) {
            s.connectionId = ownerDb->getConnectionId();
        }
        s.databaseName = node_->getName();
        // for postgres schema nodes, use the parent db name as database and schema name
        if (auto* schemaNode = dynamic_cast<PostgresSchemaNode*>(node_)) {
            if (schemaNode->parentDbNode)
                s.databaseName = schemaNode->parentDbNode->name;
            s.schemaName = schemaNode->name;
        }
    }

    if (scriptId_ == 0) {
        const int newId = appState->saveScript(s);
        if (newId > 0)
            scriptId_ = newId;
    } else {
        appState->updateScript(s);
    }
}

void SQLEditorTab::loadFromScript(const SqlScript& script) {
    scriptId_ = script.id;
    scriptName_ = script.name;
    filePath_ = script.filePath;
    std::strncpy(renameBuffer_, scriptName_.c_str(), sizeof(renameBuffer_) - 1);

    std::ifstream in(filePath_);
    if (in) {
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        setQuery(content);
    }
    contentModified_ = false;
    setName(scriptName_);
}

void SQLEditorTab::renderScriptHeader() {
    const auto& colors = Application::getInstance().getCurrentColors();

    // file icon
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::TextUnformatted(ICON_FA_FILE_CODE);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);

    if (renamingScript_) {
        // grab focus immediately on the opening frame so the SQL editor loses it at once
        if (renamingFocusNeeded_) {
            ImGui::SetKeyboardFocusHere(0);
            renamingFocusNeeded_ = false;
        }

        ImGui::SetNextItemWidth(200.0f);
        const bool committed = ImGui::InputText(
            "##script_rename", renameBuffer_, sizeof(renameBuffer_),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        // Esc cancels without committing
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            renamingScript_ = false;
        } else if (committed) {
            if (renameBuffer_[0] != '\0') {
                const std::string newName = renameBuffer_;
                if (!filePath_.empty() && std::filesystem::exists(filePath_)) {
                    // attempt the filesystem rename first; only commit on success
                    const std::string dir = getDefaultScriptsDir();
                    const std::filesystem::path newPath =
                        std::filesystem::path(dir) / (newName + ".sql");
                    std::error_code ec;
                    if (std::filesystem::exists(newPath) &&
                        !std::filesystem::equivalent(filePath_, newPath, ec)) {
                        spdlog::warn("Cannot rename: '{}' already exists", newPath.string());
                    } else {
                        std::filesystem::rename(filePath_, newPath, ec);
                        if (ec) {
                            spdlog::error("Failed to rename script file: {}", ec.message());
                        } else {
                            filePath_ = newPath.string();
                            scriptName_ = newName;
                            setName(scriptName_);
                            persistScriptToAppState();
                        }
                    }
                } else {
                    // never-saved tab: rename is purely in-memory
                    scriptName_ = newName;
                    setName(scriptName_);
                    contentModified_ = true;
                }
            }
            renamingScript_ = false;
        }

        // show the immutable ".sql" extension alongside the input
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextUnformatted(".sql");
        ImGui::PopStyleColor();
    } else {
        // display name (greyed out if not yet saved)
        const bool saved = !filePath_.empty();
        ImGui::PushStyleColor(ImGuiCol_Text, saved ? colors.text : colors.subtext0);
        ImGui::TextUnformatted(scriptName_.c_str());
        ImGui::PopStyleColor();

        ImGui::SameLine(0, Theme::Spacing::S);

        // edit icon button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(colors.surface1.x, colors.surface1.y,
                                                             colors.surface1.z, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::XS, Theme::Spacing::XS));
        if (ImGui::SmallButton(ICON_FA_PENCIL "##rename_script")) {
            std::strncpy(renameBuffer_, scriptName_.c_str(), sizeof(renameBuffer_) - 1);
            renamingScript_ = true;
            renamingFocusNeeded_ = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rename script");

        // unsaved indicator
        if (contentModified_) {
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(ICON_FA_CIRCLE "  unsaved");
            ImGui::PopStyleColor();
        } else if (!filePath_.empty()) {
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextUnformatted(filePath_.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::Separator();
}

void SQLEditorTab::renderAIPanel(float panelWidth, float availableHeight) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginChild("AIPanel", ImVec2(panelWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        // Resize handle on the left edge
        {
            constexpr float handleWidth = 4.0f;
            const ImVec2 panelPos = ImGui::GetWindowPos();
            const ImVec2 handleMin(panelPos.x, panelPos.y);

            ImGui::SetCursorScreenPos(handleMin);
            ImGui::InvisibleButton("##aiResizeHandle", ImVec2(handleWidth, availableHeight));
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
            aiChatState_->setCurrentSQL(sqlQuery);
        }
        if (aiChatPanel_) {
            aiChatPanel_->render();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
