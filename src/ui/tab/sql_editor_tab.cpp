#include "ui/tab/sql_editor_tab.hpp"
#include "IconsFontAwesome6.h"
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
#include <chrono>
#include <format>
#include <ranges>
#include <string_view>

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

    std::vector<std::string> parseMSSQLQualifierSegments(std::string_view objectName,
                                                         const std::string& databaseName) {
        std::vector<std::string> qualifiers;
        qualifiers.push_back(databaseName);

        const auto dotPos = objectName.find('.');
        if (dotPos != std::string_view::npos)
            qualifiers.push_back(std::string(objectName.substr(0, dotPos)));

        return qualifiers;
    }

    std::string getMSSQLCompletionLabel(std::string_view objectName) {
        const auto dotPos = objectName.find('.');
        if (dotPos == std::string_view::npos)
            return std::string(objectName);
        return std::string(objectName.substr(dotPos + 1));
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
} // namespace

SQLEditorTab::SQLEditorTab(const std::string& name, IDatabaseNode* node,
                           const std::string& schemaName)
    : Tab(name, TabType::SQL_EDITOR), node_(node), selectedSchemaName(schemaName) {
    sqlEditor.SetShowLineNumbers(true);
    sqlEditor.SetSubmitCallback([this] {
        sqlQuery = sqlEditor.GetText();
        startQueryExecutionAsync(sqlQuery);
    });
    bindNode(node_);
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

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);
    renderConnectionInfo();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);

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
            if (pendingEditorFocusFrames_ > 0) {
                sqlEditor.SetFocus();
                pendingEditorFocusFrames_--;
            }
            sqlEditor.Render("##SQL", ImVec2(-1, -1), true);
            sqlQuery = sqlEditor.GetText();
        }
        ImGui::EndChild();

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
    auto* dbNode = dynamic_cast<MSSQLDatabaseNode*>(node_);
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
            startQueryExecutionAsync(sqlQuery);
        }
        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(ICON_FA_ALIGN_LEFT " Format")) {
            formatSQL();
        }
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

    if (auto* dbNode = dynamic_cast<PostgresDatabaseNode*>(node); dbNode && dbNode->parentDb) {
        const std::string dbName = dbNode->name;
        binding_.resolveNode = [serverDb = dbNode->parentDb, dbName]() -> IDatabaseNode* {
            if (auto* resolved = serverDb->getDatabaseData(dbName))
                return resolved;

            if (!serverDb->areDatabasesLoaded() && !serverDb->isLoadingDatabases()) {
                serverDb->refreshDatabaseNames();
            }
            serverDb->checkDatabasesStatusAsync();
            return serverDb->getDatabaseData(dbName);
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
    }
}

void SQLEditorTab::updateCompletionKeywords() {
    std::vector<CompletionItem> items;

    // SQL keywords
    for (const auto& kw : dearsql::TextEditor::GetDefaultCompletionKeywords())
        items.push_back({kw, CompletionKind::Keyword});

    if (node_) {
        auto addColumnsFromNode = [&](IDatabaseNode* sourceNode) {
            if (!sourceNode)
                return;
            for (const auto& table : sourceNode->getTables()) {
                for (const auto& col : table.columns)
                    items.push_back({col.name, CompletionKind::Column});
            }
        };

        auto addNodeObjects = [&](IDatabaseNode* sourceNode,
                                  const std::vector<std::string>& qualifiers,
                                  bool mssqlObjectNames = false) {
            if (!sourceNode)
                return;

            for (const auto& table : sourceNode->getTables()) {
                if (mssqlObjectNames) {
                    items.push_back(makeCompletionItem(
                        getMSSQLCompletionLabel(table.name), CompletionKind::Table,
                        parseMSSQLQualifierSegments(table.name, qualifiers.front())));
                } else {
                    items.push_back(
                        makeCompletionItem(table.name, CompletionKind::Table, qualifiers));
                }
            }

            for (const auto& view : sourceNode->getViews()) {
                if (mssqlObjectNames) {
                    items.push_back(makeCompletionItem(
                        getMSSQLCompletionLabel(view.name), CompletionKind::View,
                        parseMSSQLQualifierSegments(view.name, qualifiers.front())));
                } else {
                    items.push_back(
                        makeCompletionItem(view.name, CompletionKind::View, qualifiers));
                }
            }

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
                addColumnsFromNode(schema.get());
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
                    addColumnsFromNode(schema.get());
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
                    addColumnsFromNode(dbEntry.get());
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
                scheduleMetadataLoad(dbEntry.get());
                tablesLoaded = tablesLoaded && dbEntry->isTablesLoaded();
                viewsLoaded = viewsLoaded && dbEntry->isViewsLoaded();
                addNodeObjects(dbEntry.get(), {dbEntry->name}, true);
                if (dbEntry.get() == msSqlNode)
                    addColumnsFromNode(dbEntry.get());
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
                    addColumnsFromNode(schemaEntry.get());
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
