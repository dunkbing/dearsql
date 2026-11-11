#include "ui/tab/sql_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "ai/ai_chat.hpp"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/lib_database_node.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/read_only.hpp"
#include "imgui.h"
#include "imshell/button.hpp"
#include "imshell/menu.hpp"
#include "imshell/spinner.hpp"
#include "imshell/splitter.hpp"
#include "themes.hpp"
#include "ui/ai_chat_panel.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/plan_view.hpp"
#include "ui/table_renderer.hpp"
#include "utils/app_paths.hpp"
#include "utils/sentry_utils.hpp"
#include "utils/sql_guard.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
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

    void scheduleMetadataLoad(IDatabaseNode* node) {
        if (!node)
            return;

        node->checkLoadingStatus();
        if (!node->isTablesLoaded() && !node->isLoadingTables())
            node->startTablesLoadAsync();
        if (!node->isViewsLoaded() && !node->isLoadingViews())
            node->startViewsLoadAsync();
    }

    // right-aligned popup hint: owner + type for columns, schema for relations
    std::string editorDetail(const dearsql::CompletionItem& item) {
        switch (item.kind) {
        case dearsql::CompletionKind::Column: {
            std::string text = item.owner;
            if (!item.detail.empty())
                text += (text.empty() ? "" : "  ") + item.detail;
            return text;
        }
        case dearsql::CompletionKind::Table:
        case dearsql::CompletionKind::View:
        case dearsql::CompletionKind::Sequence:
            return item.owner.empty() && item.detail == "cte" ? item.detail : item.owner;
        case dearsql::CompletionKind::Function:
            return item.detail == "function" ? std::string() : item.detail;
        case dearsql::CompletionKind::Alias:
            return item.detail;
        default:
            return {};
        }
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

} // namespace

SQLEditorTab::SQLEditorTab(const std::string& name, IDatabaseNode* node,
                           const std::string& schemaName)
    : Tab(name, TabType::SQL_EDITOR), node_(node), selectedSchemaName(schemaName),
      scriptName_(name) {
    sqlEditor.SetShowLineNumbers(true);
    // a play icon per statement: run one without selecting it
    sqlEditor.SetRunnableRanges(
        [this](std::string_view text) {
            return dearsql::statementRanges(text, node_ ? node_->getDatabaseType()
                                                        : DatabaseType::POSTGRESQL);
        },
        [this](size_t begin, size_t end) {
            startQueryExecutionAsync(sqlEditor.GetText().substr(begin, end - begin));
        });
    sqlEditor.SetSubmitCallback([this] {
        if (sqlEditor.HasSelection()) {
            startQueryExecutionAsync(sqlEditor.GetSelectedText());
        } else {
            sqlQuery = sqlEditor.GetText();
            startQueryExecutionAsync(sqlQuery);
        }
    });
    sqlEditor.SetCompletionProvider([this](std::string_view content, int cursor, bool) {
        return provideCompletions(content, cursor);
    });
    bindNode(node_);
    scheduleSyntaxCheck();
    // seed rename buffer with initial name
    std::strncpy(renameBuffer_, scriptName_.c_str(), sizeof(renameBuffer_) - 1);
}

SQLEditorTab::~SQLEditorTab() {
    // stop the query server-side and let the worker unwind on its own; the task
    // only holds the query and the executor, not the tab
    if (queryExecutionOp_.isRunning())
        ConnectionPoolBase::cancelQueriesOn(queryExecutionOp_.workerId());
    queryExecutionOp_.detach();
}

void SQLEditorTab::render() {
    // Sync editor palette with current app theme
    sqlEditor.SetPalette(
        dearsql::TextEditor::FromTheme(Application::getInstance().getCurrentColors()));
    syncBoundNodePointer();

    // until the catalog is complete, then a cheap change check once a second
    if (!completionKeywordsSet_ || ImGui::GetTime() - lastCatalogCheck_ > 1.0) {
        lastCatalogCheck_ = ImGui::GetTime();
        updateCompletionCatalog();
    }

    checkQueryExecutionStatus();
    updateSyntaxDiagnostics();
    renderParamPrompt();

    // Cmd+S / Ctrl+S save shortcut — flag here, execute after editor text is synced below
    const bool wantSave = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                          (ImGui::GetIO().KeyMods & ImGuiMod_Shortcut) &&
                          ImGui::IsKeyPressed(ImGuiKey_S, false);

    renderConnectionInfo();
    renderScriptHeader();

    constexpr float toggleStripWidth = 28.0f;
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    totalContentHeight = ImGui::GetContentRegionAvail().y;

    const float panelContentWidth = aiPanelVisible_ ? aiPanelWidth_ : 0.0f;
    float editorAreaWidth = totalWidth - toggleStripWidth - panelContentWidth;
    editorAreaWidth = std::max(200.0f, editorAreaWidth);

    // Left pane: editor + results
    if (ImGui::BeginChild("##sql_left_pane", ImVec2(editorAreaWidth, totalContentHeight), false)) {
        float paneHeight = ImGui::GetContentRegionAvail().y;
        const float editorHeight = paneHeight * splitterPosition;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition) - 4.0f;

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
                sqlEditor.ClearErrorMarker(); // its offset is stale now
                contentModified_ = true;
                scheduleSyntaxCheck();
            }
        }
        ImGui::EndChild();

        // execute save now that sqlQuery is guaranteed up-to-date
        if (wantSave) {
            saveScript();
        }

        imshell::Splitter("##sql_splitter", &splitterPosition, totalContentHeight, 100.0f, 200.0f);

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
                imshell::Spinner("##results_spinner", spinnerRadius, 2,
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
    case DatabaseType::COCKROACHDB:
        renderConnectionInfoPostgres();
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
    case DatabaseType::TIDB:
        renderConnectionInfoMySQL();
        break;
    case DatabaseType::MSSQL:
        renderConnectionInfoMSSQL();
        break;
    case DatabaseType::ORACLE:
        renderConnectionInfoOracle();
        break;
    case DatabaseType::SQLITE:
    case DatabaseType::DUCKDB:
        renderConnectionInfoSQLite();
        break;
    default:
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
        break;
    }

    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::S, Theme::Spacing::S));

    if (queryExecutionOp_.isRunning())
        ImGui::BeginDisabled();

    ImGui::SetNextItemWidth(200.0f);
    if (imshell::BeginSelect("##schema_combo", preview.c_str())) {
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
                imshell::Spinner(std::format("##loading_schemas_{}", dbName).c_str(), 5.0f, 2,
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
        imshell::EndSelect();
    }

    if (queryExecutionOp_.isRunning())
        ImGui::EndDisabled();

    ImGui::PopStyleVar();
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
    const std::string path = node_->getFullPath();
    // narrow, or mobile (a container path means nothing there): the file name, not the path
#ifdef DEARSQL_MOBILE
    constexpr bool mobile = true;
#else
    constexpr bool mobile = false;
#endif
    ImGui::Text("Database: %s", mobile || ImGui::GetContentRegionAvail().x < 560.0f
                                    ? std::filesystem::path(path).filename().string().c_str()
                                    : path.c_str());
}

void SQLEditorTab::renderDatabaseCombo(const std::string& host, const char* label,
                                       const std::string& currentName,
                                       const std::vector<std::string>& dbNames,
                                       const std::function<void(const std::string&)>& onSelect) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", host.c_str());
    ImGui::SameLine(0, Theme::Spacing::L);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine(0, Theme::Spacing::S);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::S, Theme::Spacing::S));

    if (queryExecutionOp_.isRunning())
        ImGui::BeginDisabled();

    ImGui::SetNextItemWidth(150.0f);
    if (imshell::BeginSelect("##db_combo", currentName.c_str())) {
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
        imshell::EndSelect();
    }

    if (queryExecutionOp_.isRunning())
        ImGui::EndDisabled();

    ImGui::PopStyleVar();
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

void SQLEditorTab::renderToolbar(const bool narrow) {
    const auto& colors = Application::getInstance().getCurrentColors();

    // before the buttons, so they stay put when it appears
    if (syntaxDiagnostic_.active && narrow) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.peach, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine(0, Theme::Spacing::M);
    } else if (syntaxDiagnostic_.active) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        std::string inlineMessage =
            std::string(ICON_FA_TRIANGLE_EXCLAMATION) + " " + syntaxDiagnostic_.message;
        // capped, or a long server message pushes run off the line; full text on hover
        constexpr float kMaxWidth = 320.0f;
        const bool clipped = ImGui::CalcTextSize(inlineMessage.c_str()).x > kMaxWidth;
        while (inlineMessage.size() > 4 &&
               ImGui::CalcTextSize((inlineMessage + "...").c_str()).x > kMaxWidth) {
            inlineMessage.pop_back();
            while ((inlineMessage.back() & 0xC0) == 0x80) // whole utf-8 characters
                inlineMessage.pop_back();
            if (static_cast<unsigned char>(inlineMessage.back()) >= 0xC0)
                inlineMessage.pop_back();
        }
        if (clipped)
            inlineMessage += "...";
        ImGui::TextUnformatted(inlineMessage.c_str());
        ImGui::PopStyleColor();
        if (clipped && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", syntaxDiagnostic_.message.c_str());
        ImGui::SameLine(0, Theme::Spacing::L);
    }

    if (queryExecutionOp_.isRunning()) {
        ImGui::BeginDisabled();
        imshell::IconLabelButton(ICON_FA_PLAY, "Run", imshell::ButtonVariant::Primary);
        ImGui::EndDisabled();

        ImGui::SameLine(0, Theme::Spacing::M);
        if (imshell::Button(LABEL_CANCEL)) {
            cancelQueryExecution();
        }
    } else {
        if (imshell::IconLabelButton(ICON_FA_PLAY, "Run", imshell::ButtonVariant::Primary)) {
            if (sqlEditor.HasSelection()) {
                startQueryExecutionAsync(sqlEditor.GetSelectedText());
            } else {
                startQueryExecutionAsync(sqlQuery);
            }
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        const DatabaseType type = node_ ? node_->getDatabaseType() : DatabaseType::SQLITE;
        const bool canExplain = node_ && dearsql::explainSql(type, "x", false).has_value();
        if (narrow) {
            if (imshell::BeginMenuPopup("##sql_more_menu")) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(ICON_FA_ALIGN_LEFT "  Format"))
                    formatSQL();
                if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Explain", nullptr, false,
                                    canExplain))
                    explainStatement();
                if (canExplain && dearsql::supportsExplainAnalyze(type))
                    ImGui::MenuItem("Analyze", nullptr, &explainAnalyze_);
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
            // the button last: the header measures the toolbar by the last item
            if (imshell::IconButton(ICON_FA_ELLIPSIS "##sql_more"))
                ImGui::OpenPopup("##sql_more_menu");
            return;
        }
        if (imshell::IconLabelButton(ICON_FA_ALIGN_LEFT, "Format")) {
            formatSQL();
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::BeginDisabled(!canExplain);
        if (imshell::IconLabelButton(ICON_FA_DIAGRAM_PROJECT, "Explain")) {
            explainStatement();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", canExplain ? "Show the plan of the statement under the cursor, "
                                                 "or of the selection"
                                               : "Query plans are available for PostgreSQL, MySQL "
                                                 "and SQLite");
        }
        if (canExplain && dearsql::supportsExplainAnalyze(type)) {
            ImGui::SameLine(0, Theme::Spacing::M);
            ImGui::Checkbox("Analyze###explain_analyze", &explainAnalyze_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Run the statement to show real row counts and timings. Read "
                                  "queries only");
            }
        }
    }
}

void SQLEditorTab::renderServerMessages() const {
    if (queryResult.messages.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 1.0f, 1.0f));
    for (const auto& msg : queryResult.messages) {
        ImGui::TextWrapped("%s", msg.c_str());
    }
    ImGui::PopStyleColor();
}

void SQLEditorTab::renderQueryResults() const {
    if (plan_) {
        PlanView::render(*plan_, planRaw_);
        return;
    }
    if (queryResult.empty() && queryResult.messages.empty()) {
        ImGui::Text("%s", LABEL_NO_RESULTS);
        return;
    }

    // one result shows its time on its row-count line
    if (queryResult.executionTimeMs > 0 &&
        !(queryResult.size() == 1 && queryResult.phaseTimings.empty())) {
        ImGui::Text("Execution time: %.2f ms", queryResult.executionTimeMs);
    }

    // server informational messages (e.g. SQL Server PRINT output)
    renderServerMessages();

    // only messages, no result sets (e.g. a proc that just PRINTs)
    if (queryResult.empty()) {
        return;
    }

    const bool hasTimings = !queryResult.phaseTimings.empty();

    // Single result — render directly without tabs
    if (queryResult.size() == 1 && !hasTimings) {
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
        if (hasTimings && ImGui::BeginTabItem("Timing###TimingTab")) {
            renderTimingWaterfall();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void SQLEditorTab::renderTimingWaterfall() const {
    const auto& phases = queryResult.phaseTimings;
    double total = 0.0;
    for (const auto& [name, ms] : phases) {
        total += ms;
    }
    if (total <= 0.0) {
        return;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Total: %.2f ms", total);
    ImGui::Spacing();

    // right-aligned label column sized to the widest "NAME • 1.234ms"
    float labelWidth = 0.0f;
    std::vector<std::string> labels;
    labels.reserve(phases.size());
    for (const auto& [name, ms] : phases) {
        labels.push_back(std::format("{} \xE2\x80\xA2 {:.3f}ms", name, ms));
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(labels.back().c_str()).x);
    }
    labelWidth += Theme::Spacing::L;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + Theme::Spacing::S;
    const float barHeight = ImGui::GetTextLineHeight();
    const float chartWidth =
        std::max(ImGui::GetContentRegionAvail().x - labelWidth - Theme::Spacing::L, 100.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImU32 barColor = ImGui::GetColorU32(ImVec4(0.42f, 0.40f, 0.75f, 1.0f));
    const ImU32 labelColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    double elapsed = 0.0;
    for (size_t i = 0; i < phases.size(); ++i) {
        const float y = origin.y + static_cast<float>(i) * rowHeight;
        const float textW = ImGui::CalcTextSize(labels[i].c_str()).x;
        drawList->AddText(ImVec2(origin.x + labelWidth - Theme::Spacing::L - textW,
                                 y + (barHeight - ImGui::GetTextLineHeight()) * 0.5f),
                          labelColor, labels[i].c_str());

        const float x0 = origin.x + labelWidth + static_cast<float>(elapsed / total) * chartWidth;
        const float w = std::max(static_cast<float>(phases[i].second / total) * chartWidth, 2.0f);
        drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w, y + barHeight), barColor);
        elapsed += phases[i].second;
    }
    ImGui::Dummy(ImVec2(0.0f, static_cast<float>(phases.size()) * rowHeight));
}

void SQLEditorTab::renderSingleResult(const StatementResult& r, size_t index) const {
    if (!r.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", r.errorMessage.c_str());
        return;
    }

    // "347 rows  0.53 ms": the time rides along when this is the only result
    const std::string took = queryResult.size() == 1 && queryResult.phaseTimings.empty() &&
                                     queryResult.executionTimeMs > 0
                                 ? std::format("  {:.2f} ms", queryResult.executionTimeMs)
                                 : "";

    if (r.columnNames.empty()) {
        // DML/DDL result
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s%s", r.message.c_str(), took.c_str());
        return;
    }

    // SELECT result
    if (r.tableData.empty()) {
        ImGui::Text("%s%s", LABEL_NO_ROWS, took.c_str());
        return;
    }

    // the grid (or its json), then the view tabs and "347 rows  0.53 ms" under it
    const float footer = ImGui::GetFrameHeight() + Theme::Spacing::S;
    const float gridHeight = std::max(ImGui::GetContentRegionAvail().y - footer, 50.0f);
    const std::string tableId = "QueryResults_" + std::to_string(index);
    if (ImGui::BeginChild((tableId + "_grid").c_str(), ImVec2(0, gridHeight))) {
        if (resultView_ == 1) {
            jsonView_.render("##result_json", r.columnNames, r.tableData);
        } else if (resultView_ == 2) {
            chart_.render("##result_chart", r.columnNames, r.tableData);
        } else {
            TableRenderer::Config config;
            config.allowEditing = false;
            config.showRowNumbers = false;
            TableRenderer tableRenderer(config);
            tableRenderer.setColumns(r.columnNames);
            tableRenderer.setData(r.tableData);
            tableRenderer.render(tableId.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));
    imshell::SegmentedTabs("##result_view", {"Data", "JSON", "Chart"}, &resultView_);

    const std::string status =
        std::format("{} {}{}", r.tableData.size(), r.tableData.size() == 1 ? "row" : "rows", took);
    const bool limited = static_cast<int>(r.tableData.size()) >= MAX_QUERY_ROWS;
    float width = ImGui::CalcTextSize(status.c_str()).x;
    if (limited)
        width += ImGui::CalcTextSize(LABEL_ROW_LIMIT).x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - width));
    ImGui::AlignTextToFramePadding();
    if (limited) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", LABEL_ROW_LIMIT);
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(status.c_str());
}

void SQLEditorTab::startQueryExecutionAsync(const std::string& query,
                                            std::optional<PendingExplain> explain) {
    if (queryExecutionOp_.isRunning() || paramPrompt_) {
        return;
    }
    const auto type = node_ ? node_->getDatabaseType() : DatabaseType::POSTGRESQL;
    if (auto names = dearsql::queryParameters(query, type); !names.empty()) {
        paramPrompt_ = ParamPrompt{query, std::move(names), explain};
        return;
    }
    runQueryNow(query, explain);
}

void SQLEditorTab::runQueryNow(const std::string& query, std::optional<PendingExplain> explain) {
    if (queryExecutionOp_.isRunning()) {
        return;
    }

    queryError.clear();
    lastQueryDuration = std::chrono::milliseconds{0};
    pendingExplain_ = explain;
    plan_.reset();

    syncBoundNodePointer();

    if (auto reason = ReadOnly::rejectReason(node_, query); !reason.empty()) {
        showError(std::move(reason));
        return;
    }

    IQueryExecutor* executor = nullptr;
    if (binding_.resolveExecutor) {
        executor = binding_.resolveExecutor();
    }

    if (executor) {
        auto keep = keepOwnerAlive(dynamic_cast<IDatabaseNode*>(executor));
        queryExecutionOp_.startCancellable(
            [query, executor, keep = std::move(keep)](const std::stop_token& stopToken) {
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
        return;
    }
    showError(LABEL_NO_DATABASE_SELECTED);
}

void SQLEditorTab::showError(std::string message) {
    StatementResult r;
    r.success = false;
    r.errorMessage = std::move(message);
    queryResult = QueryResult{};
    queryResult.statements.push_back(std::move(r));
}

void SQLEditorTab::explainStatement() {
    if (!node_ || queryExecutionOp_.isRunning()) {
        return;
    }
    const DatabaseType type = node_->getDatabaseType();
    const bool analyze = explainAnalyze_ && dearsql::supportsExplainAnalyze(type);
    const bool fromSelection = sqlEditor.HasSelection();
    const std::string source = fromSelection ? sqlEditor.GetSelectedText() : sqlQuery;
    const size_t offset =
        fromSelection ? 0 : static_cast<size_t>(std::max(sqlEditor.GetCursorIndex(), 0));
    auto [begin, end] = dearsql::statementRangeAt(source, offset, type);
    // the cursor just after a closing ';': the statement it closes
    if (begin == end && !fromSelection) {
        size_t back = std::min(offset, source.size());
        while (back > 0 && (std::isspace(static_cast<unsigned char>(source[back - 1])) ||
                            source[back - 1] == ';')) {
            --back;
        }
        if (back > 0) {
            std::tie(begin, end) = dearsql::statementRangeAt(source, back - 1, type);
        }
    }
    const std::string statement = source.substr(begin, end - begin);
    plan_.reset();
    // analyze runs what it is given: a second statement in the selection would run too
    if (fromSelection && source.find_first_not_of(" \t\r\n;", end) != std::string::npos) {
        showError("Select a single statement to explain.");
        return;
    }
    if (analyze && !SqlGuard::isReadOnly(statement)) {
        showError("Analyze runs the statement, so it is only offered for read queries. Turn off "
                  "Analyze to see the plan of this one.");
        return;
    }
    const auto sql = dearsql::explainSql(type, statement, analyze);
    if (!sql) {
        showError("Put the cursor in a statement to explain.");
        return;
    }
    startQueryExecutionAsync(*sql, PendingExplain{type, analyze});
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
            // lookup only; a dropped database must not be recreated
            auto find = [&]() -> IDatabaseNode* {
                const auto& nodes = std::as_const(*serverDb).getDatabaseDataMap();
                auto it = nodes.find(dbName);
                return it != nodes.end() ? it->second.get() : nullptr;
            };
            if (auto* resolved = find()) {
                return resolved;
            }

            if (!serverDb->areDatabasesLoaded() && !serverDb->isLoadingDatabases()) {
                serverDb->refreshDatabaseNames();
            }
            serverDb->checkDatabasesStatusAsync();
            return find();
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
        // captures the server and names, never the schema node: a relist or a
        // drop destroys it while this tab lives
        binding_.resolveNode = [serverDb = schemaNode->parentDbNode->parentDb, dbName,
                                schemaName]() -> IDatabaseNode* {
            // lookup only; a dropped database must not be recreated
            const auto& nodes = std::as_const(*serverDb).getDatabaseDataMap();
            auto dbIt = nodes.find(dbName);
            auto* dbNode = dbIt != nodes.end() ? dbIt->second.get() : nullptr;
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

            if (pendingExplain_ && !result.statements.empty()) {
                plan_ = dearsql::parsePlan(pendingExplain_->type, result.statements.front(),
                                           pendingExplain_->analyze);
            }
            pendingExplain_.reset();
            lastQueryDuration =
                std::chrono::milliseconds{static_cast<long long>(result.executionTimeMs)};
            queryResult = std::move(result);
        });
    } catch (const std::exception& e) {
        queryError = "Error in async query execution: " + std::string(e.what());
    }
}

void SQLEditorTab::cancelQueryExecution() {
    // the stop token alone never reaches a query blocked in the driver
    if (queryExecutionOp_.isRunning())
        ConnectionPoolBase::cancelQueriesOn(queryExecutionOp_.workerId());
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
    // a server check is a round trip: wait for a longer pause in typing
    syntaxCheckDelay_ =
        node_ && dearsql::hasServerSyntaxCheck(node_->getDatabaseType()) ? 0.8f : 0.25f;
}

void SQLEditorTab::updateSyntaxDiagnostics() {
    // a server answer for text that has changed since is dropped: a newer check follows
    serverSyntaxOp_.check([this](std::pair<std::string, std::optional<dearsql::SyntaxError>> r) {
        if (r.first == sqlQuery) {
            showSyntaxError(r.second);
        }
    });
    if (!syntaxCheckPending_) {
        return;
    }
    syntaxCheckDelay_ -= ImGui::GetIO().DeltaTime;
    if (syntaxCheckDelay_ > 0.0f) {
        return;
    }
    if (!node_ || trimSqlView(sqlQuery).empty()) {
        syntaxCheckPending_ = false;
        showSyntaxError(std::nullopt);
        return;
    }
    const auto type = node_->getDatabaseType();
    if (!dearsql::hasServerSyntaxCheck(type)) {
        // the engine's own parser, so a valid query is never flagged; engines without
        // one (mysql, oracle...) are not checked at all
        syntaxCheckPending_ = false;
        showSyntaxError(dearsql::checkSyntax(sqlQuery, type));
        return;
    }
    auto* lib = dynamic_cast<LibDatabaseNode*>(node_);
    if (!lib || serverSyntaxOp_.isRunning()) {
        syntaxCheckPending_ = lib != nullptr; // one at a time: retry when it lands
        return;
    }
    syntaxCheckPending_ = false;
    serverSyntaxOp_.start([lib, keep = keepOwnerAlive(node_), text = sqlQuery] {
        std::optional<dearsql::SyntaxError> error;
        try {
            error = lib->withHandle([&](dearsql::IDatabase& db) { return db.checkSyntax(text); });
        } catch (const std::exception& e) {
            spdlog::debug("syntax check: {}", e.what()); // no connection: no check
        }
        return std::pair{text, error};
    });
}

void SQLEditorTab::showSyntaxError(const std::optional<dearsql::SyntaxError>& error) {
    syntaxDiagnostic_ = {};
    if (!error) {
        sqlEditor.ClearErrorMarker();
        return;
    }
    const std::string_view before = std::string_view(sqlQuery).substr(0, error->offset);
    syntaxDiagnostic_.active = true;
    syntaxDiagnostic_.line = 1 + static_cast<int>(std::ranges::count(before, '\n'));
    syntaxDiagnostic_.column = 1 + static_cast<int>(before.size() - (before.rfind('\n') + 1));
    syntaxDiagnostic_.message = std::format("Line {}: {}", syntaxDiagnostic_.line, error->message);
    sqlEditor.SetErrorMarker(error->offset, error->message);
}

void SQLEditorTab::updateCompletionCatalog() {
    if (!node_)
        return;

    // every node whose objects can be named from this editor, with the
    // qualifier path a user types before them
    struct Source {
        IDatabaseNode* node;
        std::string schema;
    };
    std::vector<Source> sources;
    std::string defaultSchema;
    bool loaded = true;

    auto addSource = [&](IDatabaseNode* source, std::string schema) {
        if (!source)
            return;
        scheduleMetadataLoad(source);
        loaded = loaded && source->isTablesLoaded() && source->isViewsLoaded();
        sources.push_back({source, std::move(schema)});
    };

    // mssql: every schema of every database, "db.schema"
    auto addMssqlServer = [&](MSSQLDatabase* serverDb) {
        if (!serverDb)
            return;
        serverDb->checkDatabasesStatusAsync();
        for (const auto& dbEntry : serverDb->getDatabaseDataMap() | std::views::values) {
            if (!dbEntry)
                continue;
            dbEntry->checkSchemasStatusAsync();
            if (!dbEntry->schemasLoaded) {
                if (!dbEntry->schemasLoader.isRunning())
                    dbEntry->startSchemasLoadAsync();
                loaded = false;
                continue;
            }
            for (const auto& schema : dbEntry->schemas) {
                if (schema)
                    addSource(schema.get(), dbEntry->name + "." + schema->name);
            }
        }
    };

    if (auto* dbNode = dynamic_cast<PostgresDatabaseNode*>(node_); dbNode) {
        dbNode->checkSchemasStatusAsync();
        if (!dbNode->schemasLoaded && !dbNode->schemasLoader.isRunning())
            dbNode->startSchemasLoadAsync();
        loaded = dbNode->schemasLoaded;
        for (const auto& schema : dbNode->schemas) {
            if (schema)
                addSource(schema.get(), schema->name);
        }
        defaultSchema = "public";
    } else if (auto* schemaNode = dynamic_cast<PostgresSchemaNode*>(node_);
               schemaNode && schemaNode->parentDbNode) {
        auto* parent = schemaNode->parentDbNode;
        parent->checkSchemasStatusAsync();
        if (!parent->schemasLoaded && !parent->schemasLoader.isRunning())
            parent->startSchemasLoadAsync();
        loaded = parent->schemasLoaded;
        for (const auto& schema : parent->schemas) {
            if (schema)
                addSource(schema.get(), schema->name);
        }
        defaultSchema = schemaNode->name;
    } else if (auto* mySqlNode = dynamic_cast<MySQLDatabaseNode*>(node_);
               mySqlNode && mySqlNode->parentDb) {
        mySqlNode->parentDb->checkDatabasesStatusAsync();
        for (const auto& dbEntry : mySqlNode->parentDb->getDatabaseDataMap() | std::views::values)
            addSource(dbEntry.get(), dbEntry ? dbEntry->name : std::string());
        defaultSchema = mySqlNode->name;
    } else if (auto* msSqlSchemaNode = dynamic_cast<MSSQLSchemaNode*>(node_);
               msSqlSchemaNode && msSqlSchemaNode->parentDbNode) {
        addMssqlServer(msSqlSchemaNode->parentDbNode->parentDb);
        defaultSchema = msSqlSchemaNode->parentDbNode->name + "." + msSqlSchemaNode->name;
    } else if (auto* msSqlNode = dynamic_cast<MSSQLDatabaseNode*>(node_);
               msSqlNode && msSqlNode->parentDb) {
        addMssqlServer(msSqlNode->parentDb);
        defaultSchema = msSqlNode->name + ".dbo";
    } else if (auto* oracleNode = dynamic_cast<OracleDatabaseNode*>(node_);
               oracleNode && oracleNode->parentDb) {
        oracleNode->parentDb->checkDatabasesStatusAsync();
        for (const auto& schemaEntry :
             oracleNode->parentDb->getDatabaseDataMap() | std::views::values)
            addSource(schemaEntry.get(), schemaEntry ? schemaEntry->name : std::string());
        defaultSchema = oracleNode->name;
    } else {
        addSource(node_, {});
    }

    // rebuild only when a loaded list changed (first load, refresh, new schema)
    size_t signature = std::hash<std::string>{}(defaultSchema);
    auto mix = [&](size_t v) { signature ^= v + 0x9e3779b97f4a7c15ULL + (signature << 6); };
    for (const auto& s : sources) {
        mix(reinterpret_cast<uintptr_t>(s.node));
        mix(reinterpret_cast<uintptr_t>(s.node->getTables().data()));
        mix(s.node->getTables().size());
        mix(reinterpret_cast<uintptr_t>(s.node->getViews().data()));
        mix(s.node->getViews().size());
        mix(s.node->getSequences().size());
    }
    completionType_ = node_->getDatabaseType();
    if (signature != completionSignature_) {
        completionSignature_ = signature;
        dearsql::CompletionCatalog catalog;
        catalog.defaultSchema = defaultSchema;
        for (const auto& s : sources) {
            if (!s.schema.empty())
                catalog.schemas.push_back(s.schema);
            for (Table table : s.node->getTables()) {
                table.schema = s.schema;
                catalog.tables.push_back(std::move(table));
            }
            for (Table view : s.node->getViews()) {
                view.schema = s.schema;
                catalog.views.push_back(std::move(view));
            }
            for (const auto& seq : s.node->getSequences())
                catalog.sequences.push_back({s.schema, seq});
        }
        completionCatalog_ = std::move(catalog);
    }
    completionKeywordsSet_ = loaded;
}

dearsql::TextEditor::CompletionResponse SQLEditorTab::provideCompletions(std::string_view content,
                                                                         int cursor) const {
    const auto result = dearsql::complete(content, static_cast<size_t>(std::max(cursor, 0)),
                                          completionCatalog_, completionType_);
    dearsql::TextEditor::CompletionResponse response;
    response.replaceStart = static_cast<int>(result.replaceStart);
    response.replaceEnd = static_cast<int>(result.replaceEnd);
    response.items.reserve(result.items.size());
    for (const auto& item : result.items) {
        auto editorItem = dearsql::TextEditor::Row(item.label, item.kind);
        editorItem.insertText = item.insertText;
        editorItem.detailText = editorDetail(item);
        response.items.push_back(std::move(editorItem));
    }
    return response;
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
    const std::filesystem::path dir = AppPaths::dataDir() / "scripts";
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
    const bool narrow = ImGui::GetContentRegionAvail().x < 560.0f;
    // same button height as the settings dialog
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::Spacing::M, Theme::Spacing::S));
    ImGui::AlignTextToFramePadding();

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
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        if (imshell::IconButton(ICON_FA_PENCIL "##rename_script", imshell::ButtonVariant::Secondary,
                               ImVec2(0.0f, 0.0f), imshell::ButtonSize::Small)) {
            std::strncpy(renameBuffer_, scriptName_.c_str(), sizeof(renameBuffer_) - 1);
            renamingScript_ = true;
            renamingFocusNeeded_ = true;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rename script");

        // unsaved indicator
        if (contentModified_) {
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(ICON_FA_CIRCLE "  unsaved");
            ImGui::PopStyleColor();
        } else if (!filePath_.empty() && !narrow) {
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextUnformatted(filePath_.c_str());
            ImGui::PopStyleColor();
        }
    }

    // run, format, explain: right-aligned on the header line (width from last frame)
    ImGui::SameLine();
    const float lineEnd = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), lineEnd - toolbarWidth_));
    const float toolbarStart = ImGui::GetCursorScreenPos().x;
    renderToolbar(narrow);
    toolbarWidth_ = ImGui::GetItemRectMax().x - toolbarStart;
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
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

void SQLEditorTab::renderParamPrompt() {
    if (!paramPrompt_) {
        return;
    }
    constexpr const char* popupId = "Query parameters###query_params";
    if (!ImGui::IsPopupOpen(popupId)) {
        ImGui::OpenPopup(popupId);
    }
    ImGui::SetNextWindowSize(imshell::FitScreen(ImVec2(440.0f, 0.0f)), ImGuiCond_Appearing);
    imshell::CapToScreen();
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    bool run = false;
    float labelWidth = 0.0f;
    for (const auto& name : paramPrompt_->names) {
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize((":" + name).c_str()).x);
    }
    for (size_t i = 0; i < paramPrompt_->names.size(); ++i) {
        const auto& name = paramPrompt_->names[i];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((":" + name).c_str());
        ImGui::SameLine(labelWidth + Theme::Spacing::L * 2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (static_cast<int>(i) == paramPrompt_->focus) {
            ImGui::SetKeyboardFocusHere();
            paramPrompt_->focus = -1;
        }
        auto& buf = paramValues_[name];
        if (ImGui::InputText(std::format("##param_{}", name).c_str(), buf.data(), buf.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // enter moves on, and runs from the last field
            if (i + 1 < paramPrompt_->names.size()) {
                paramPrompt_->focus = static_cast<int>(i + 1);
            } else {
                run = true;
            }
        }
    }
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::TextWrapped("Numbers and NULL go in as typed, anything else as a quoted string.");
    ImGui::PopStyleColor();

    run |= imshell::Button("Run###param_run", imshell::ButtonVariant::Primary);
    ImGui::SameLine();
    const bool cancel =
        ImGui::Button("Cancel###param_cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::PopStyleVar();
    if (run || cancel) {
        auto prompt = std::move(*paramPrompt_);
        paramPrompt_.reset();
        ImGui::CloseCurrentPopup();
        if (run) {
            std::map<std::string, std::string> values;
            for (const auto& name : prompt.names) {
                values[name] = paramValues_[name].data();
            }
            const auto type = node_ ? node_->getDatabaseType() : DatabaseType::POSTGRESQL;
            runQueryNow(dearsql::bindParameters(prompt.query, type, values), prompt.explain);
        }
    }
    ImGui::EndPopup();
}
