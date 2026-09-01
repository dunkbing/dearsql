#include "ui/ai_sidebar_panel.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/cassandra.hpp"
#include "database/database_node.hpp"
#include "database/file_database.hpp"
#include "database/mongodb.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "database/mssql.hpp"
#include "database/mssql/mssql_database_node.hpp"
#include "database/mysql.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/oracle.hpp"
#include "database/oracle/oracle_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/markdown_text.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace {
    struct ApiModelOption {
        const char* label;
        const char* model;
        AIProvider provider;
    };
    constexpr ApiModelOption API_MODELS[] = {
        {"claude-sonnet-4-6", "claude-sonnet-4-6", AIProvider::ANTHROPIC},
        {"claude-haiku-4-5", "claude-haiku-4-5-20251001", AIProvider::ANTHROPIC},
        {"gemini-2.5-flash", "gemini-2.5-flash", AIProvider::GEMINI},
        {"gemini-2.5-pro", "gemini-2.5-pro", AIProvider::GEMINI},
    };
    constexpr int API_MODEL_COUNT = sizeof(API_MODELS) / sizeof(API_MODELS[0]);
    constexpr int MENTION_MAX_VISIBLE = 8;

    const char* dbTypeLabel(DatabaseType type) {
        switch (type) {
        case DatabaseType::SQLITE:
            return "SQLite";
        case DatabaseType::DUCKDB:
            return "DuckDB";
        case DatabaseType::POSTGRESQL:
            return "PostgreSQL";
        case DatabaseType::REDSHIFT:
            return "Redshift";
        case DatabaseType::MYSQL:
            return "MySQL";
        case DatabaseType::MARIADB:
            return "MariaDB";
        case DatabaseType::MONGODB:
            return "MongoDB";
        case DatabaseType::REDIS:
            return "Redis";
        case DatabaseType::MSSQL:
            return "SQL Server";
        case DatabaseType::ORACLE:
            return "Oracle";
        case DatabaseType::CASSANDRA:
            return "Cassandra";
        }
        return "SQL";
    }

    std::string homeDir() {
        const char* home = std::getenv("HOME");
        return home && *home ? home : ".";
    }

    bool isWordChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
    }

    std::string tableDdlText(const Table& table, bool isView) {
        std::string out = std::format("{} {}\n", isView ? "View" : "Table", table.name);
        for (const auto& col : table.columns) {
            out += "  " + col.name + " " + col.type;
            if (col.isPrimaryKey) {
                out += " PRIMARY KEY";
            }
            if (col.isNotNull) {
                out += " NOT NULL";
            }
            out += "\n";
        }
        for (const auto& fk : table.foreignKeys) {
            out += std::format("  FOREIGN KEY {} -> {}.{}\n", fk.sourceColumn, fk.targetTable,
                               fk.targetColumn);
        }
        return out;
    }
} // namespace

AISidebarPanel::AISidebarPanel()
    : apiClient_(std::make_unique<AIClient>()), apiChat_(std::make_unique<AIChatState>(nullptr)) {}

AISidebarPanel::~AISidebarPanel() = default;

// ---------------------------------------------------------------- backends

bool AISidebarPanel::isAcpBackend() const {
    return backendIndex_ <= static_cast<int>(AcpAgents::catalog().size());
}

const AcpAgentDef* AISidebarPanel::currentAgentDef() const {
    const auto& cat = AcpAgents::catalog();
    if (backendIndex_ >= 0 && backendIndex_ < static_cast<int>(cat.size())) {
        return &cat[static_cast<size_t>(backendIndex_)];
    }
    return nullptr; // custom or api key
}

std::vector<std::string> AISidebarPanel::currentInvocation(std::string& missingReason) {
    if (const AcpAgentDef* def = currentAgentDef()) {
        if (auto inv = AcpAgents::resolveInvocation(*def)) {
            return *inv;
        }
        missingReason = std::format("{} is not installed (looked for `{}` and `npx` on your PATH).",
                                    def->name, def->runCmd.front());
        return {};
    }
    // custom command: split on spaces (quoted args unsupported)
    std::vector<std::string> argv;
    std::string cur;
    for (const char* p = customCmdBuf_; *p; ++p) {
        if (*p == ' ') {
            if (!cur.empty()) {
                argv.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) {
        argv.push_back(std::move(cur));
    }
    if (argv.empty()) {
        missingReason = "Enter a custom agent command first.";
    }
    return argv;
}

void AISidebarPanel::ensureSettingsLoaded() {
    if (settingsLoaded_) {
        return;
    }
    auto* appState = Application::getInstance().getAppState();
    const auto& cat = AcpAgents::catalog();
    const std::string backend = appState->getSetting("ai_sidebar_backend", cat.front().id);
    backendIndex_ = 0;
    for (size_t i = 0; i < cat.size(); ++i) {
        if (cat[i].id == backend) {
            backendIndex_ = static_cast<int>(i);
        }
    }
    if (backend == "custom") {
        backendIndex_ = static_cast<int>(cat.size());
    } else if (backend == "api") {
        backendIndex_ = static_cast<int>(cat.size()) + 1;
    }
    const std::string custom = appState->getSetting("ai_custom_agent_cmd", "");
    std::strncpy(customCmdBuf_, custom.c_str(), sizeof(customCmdBuf_) - 1);
    mcpEnabled_ = appState->getSetting("ai_mcp_enabled", "1") == "1";
    settingsLoaded_ = true;
}

void AISidebarPanel::switchBackend(int newIndex) {
    if (newIndex == backendIndex_) {
        return;
    }
    backendIndex_ = newIndex;
    stopAgent();
    agentMissing_ = false;
    agentMissingReason_.clear();
    sentSchemaContext_ = false;

    auto* appState = Application::getInstance().getAppState();
    const auto& cat = AcpAgents::catalog();
    std::string id = "api";
    if (backendIndex_ < static_cast<int>(cat.size())) {
        id = cat[static_cast<size_t>(backendIndex_)].id;
    } else if (backendIndex_ == static_cast<int>(cat.size())) {
        id = "custom";
    }
    appState->setSetting("ai_sidebar_backend", id);
}

void AISidebarPanel::stopAgent() {
    acp_.reset();
    pendingPromptText_.clear();
}

// ---------------------------------------------------------------- context

std::vector<AISidebarPanel::NodeRef> AISidebarPanel::collectNodes() const {
    std::vector<NodeRef> nodes;
    auto db = Application::getInstance().getSelectedDatabase();
    if (!db || !db->isConnected()) {
        return nodes;
    }
    const auto type = db->getConnectionInfo().type;

    if (isFileDatabase(type)) {
        if (auto* fileDb = dynamic_cast<FileDatabase*>(db.get())) {
            nodes.push_back({db->getConnectionInfo().name, fileDb});
        }
        return nodes;
    }

    auto collect = [&nodes]<typename T>(T* server) {
        if (!server) {
            return;
        }
        for (auto& [name, node] : server->getDatabaseDataMap()) {
            if constexpr (std::is_same_v<T, PostgresDatabase>) {
                if (node->schemasLoaded && !node->schemas.empty()) {
                    for (const auto& schema : node->schemas) {
                        nodes.push_back({name + "." + schema->getName(), schema.get()});
                    }
                    continue;
                }
            }
            nodes.push_back({name, node.get()});
        }
    };

    switch (type) {
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT:
        collect(dynamic_cast<PostgresDatabase*>(db.get()));
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        collect(dynamic_cast<MySQLDatabase*>(db.get()));
        break;
    case DatabaseType::MONGODB:
        collect(dynamic_cast<MongoDBDatabase*>(db.get()));
        break;
    case DatabaseType::MSSQL:
        collect(dynamic_cast<MSSQLDatabase*>(db.get()));
        break;
    case DatabaseType::ORACLE:
        collect(dynamic_cast<OracleDatabase*>(db.get()));
        break;
    case DatabaseType::CASSANDRA:
        collect(dynamic_cast<CassandraDatabase*>(db.get()));
        break;
    default:
        break; // redis: no schema nodes
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const NodeRef& a, const NodeRef& b) { return a.label < b.label; });
    return nodes;
}

IDatabaseNode* AISidebarPanel::contextNode() {
    const auto nodes = collectNodes();
    if (nodes.empty()) {
        return nullptr;
    }
    if (contextNodeIndex_ >= static_cast<int>(nodes.size())) {
        contextNodeIndex_ = 0;
    }
    return nodes[static_cast<size_t>(contextNodeIndex_)].node;
}

void AISidebarPanel::syncContext() {
    auto db = Application::getInstance().getSelectedDatabase();
    if (db.get() != lastDb_) {
        lastDb_ = db.get();
        contextNodeIndex_ = 0;
        sentSchemaContext_ = false;
        mentionIndex_.clear();
    }
    // keep the mcp tool pointed at the current node
    const auto nodes = collectNodes();
    if (nodes.empty()) {
        mcp_.setNode(nullptr, "");
    } else {
        if (contextNodeIndex_ >= static_cast<int>(nodes.size())) {
            contextNodeIndex_ = 0;
        }
        const auto& ref = nodes[static_cast<size_t>(contextNodeIndex_)];
        mcp_.setNode(ref.node, ref.label);
    }
}

std::string AISidebarPanel::schemaOverview(IDatabaseNode* node) const {
    if (!node || !node->isTablesLoaded()) {
        return "(schema not loaded yet)";
    }
    std::string out;
    for (const auto& table : node->getTables()) {
        out += std::format("Table {} (", table.name);
        for (size_t i = 0; i < table.columns.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += table.columns[i].name + " " + table.columns[i].type;
        }
        out += ")\n";
        if (out.size() > 16 * 1024) {
            out += "...\n";
            return out;
        }
    }
    if (node->isViewsLoaded()) {
        for (const auto& view : node->getViews()) {
            out += std::format("View {}\n", view.name);
        }
    }
    return out;
}

void AISidebarPanel::rebuildMentionIndex() {
    mentionIndex_.clear();
    for (const auto& ref : collectNodes()) {
        if (!ref.node || !ref.node->isTablesLoaded()) {
            continue;
        }
        for (const auto& table : ref.node->getTables()) {
            if (table.name.find(' ') != std::string::npos) {
                continue; // ponytail: no quoting for names with spaces
            }
            mentionIndex_.push_back({table.name, table.name + "  (" + ref.label + ")", ref.node});
        }
        if (ref.node->isViewsLoaded()) {
            for (const auto& view : ref.node->getViews()) {
                if (view.name.find(' ') != std::string::npos) {
                    continue;
                }
                mentionIndex_.push_back(
                    {view.name, view.name + "  (" + ref.label + ", view)", ref.node});
            }
        }
    }
}

json AISidebarPanel::buildPromptBlocks(const std::string& text) {
    json blocks = json::array();

    if (!sentSchemaContext_) {
        IDatabaseNode* node = contextNode();
        std::string intro = "You are helping with a ";
        if (node) {
            intro += std::string(dbTypeLabel(node->getDatabaseType())) + " database";
        } else {
            intro += "database (none selected yet)";
        }
        intro += " inside DearSQL, a desktop SQL client. Prefer answering with SQL in "
                 "```sql code blocks. Do not modify files; if a DearSQL MCP tool is "
                 "available, you may use it to inspect schema and run read-only queries.";
        blocks.push_back({{"type", "text"}, {"text", intro}});
        if (node) {
            blocks.push_back({{"type", "resource"},
                              {"resource",
                               {{"uri", "dearsql://schema"},
                                {"mimeType", "text/plain"},
                                {"text", schemaOverview(node)}}}});
        }
        sentSchemaContext_ = true;
    }

    blocks.push_back({{"type", "text"}, {"text", text}});

    // resolve @mentions into embedded table definitions
    rebuildMentionIndex();
    size_t i = 0;
    std::vector<std::string> seen;
    while ((i = text.find('@', i)) != std::string::npos) {
        size_t end = i + 1;
        while (end < text.size() && isWordChar(text[end])) {
            ++end;
        }
        const std::string name = text.substr(i + 1, end - i - 1);
        i = end;
        if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) {
            continue;
        }
        for (const auto& entry : mentionIndex_) {
            if (entry.name != name || !entry.node) {
                continue;
            }
            const Table* found = nullptr;
            bool isView = false;
            for (const auto& t : entry.node->getTables()) {
                if (t.name == name) {
                    found = &t;
                }
            }
            if (!found && entry.node->isViewsLoaded()) {
                for (const auto& v : entry.node->getViews()) {
                    if (v.name == name) {
                        found = &v;
                        isView = true;
                    }
                }
            }
            if (found) {
                blocks.push_back({{"type", "resource"},
                                  {"resource",
                                   {{"uri", "dearsql://table/" + name},
                                    {"mimeType", "text/plain"},
                                    {"text", tableDdlText(*found, isView)}}}});
                seen.push_back(name);
            }
            break;
        }
    }
    return blocks;
}

// ---------------------------------------------------------------- sending

void AISidebarPanel::sendMessage() {
    std::string text(inputBuf_);
    const auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return;
    }
    text = text.substr(start, text.find_last_not_of(" \t\n\r") - start + 1);

    items_.push_back({.kind = Item::Kind::User, .text = text});
    inputBuf_[0] = '\0';
    cursorPos_ = 0;
    mentionOpen_ = false;
    scrollToBottom_ = true;
    focusInput_ = true;

    if (isAcpBackend()) {
        sendAcp(text);
    } else {
        sendApi(text);
    }
}

void AISidebarPanel::sendAcp(const std::string& text) {
    if (acp_ && acp_->isSessionReady()) {
        acp_->prompt(buildPromptBlocks(text));
        return;
    }
    if (acp_ && acp_->isRunning()) {
        pendingPromptText_ = text; // session still being created
        return;
    }

    std::string reason;
    const auto argv = currentInvocation(reason);
    if (argv.empty()) {
        agentMissing_ = true;
        agentMissingReason_ = reason;
        pendingPromptText_ = text;
        return;
    }
    agentMissing_ = false;

    std::string mcpUrl;
    if (mcpEnabled_) {
        if (mcp_.start()) {
            mcpUrl = mcp_.url();
        }
        syncContext();
    }

    acp_ = std::make_unique<AcpClient>();
    auto [ok, err] = acp_->start(argv, homeDir(), mcpUrl, "dearsql");
    if (!ok) {
        items_.push_back({.kind = Item::Kind::Error, .text = err});
        acp_.reset();
        return;
    }
    pendingPromptText_ = text;
}

void AISidebarPanel::sendApi(const std::string& text) {
    auto* appState = Application::getInstance().getAppState();
    const AIProvider provider = API_MODELS[apiModelIndex_].provider;
    std::string apiKey = appState->getSetting(
        provider == AIProvider::GEMINI ? "ai_api_key_gemini" : "ai_api_key_anthropic", "");
    if (apiKey.empty()) {
        apiKey = appState->getSetting("ai_api_key", "");
    }
    if (apiKey.empty()) {
        items_.push_back({.kind = Item::Kind::Error,
                          .text = "No API key configured — open AI Settings, or switch to a "
                                  "coding agent above."});
        return;
    }

    // request history: user/assistant turns only
    std::vector<AIChatMessage> request;
    for (const auto& item : items_) {
        if (item.kind == Item::Kind::User) {
            request.push_back({"user", item.text});
        } else if (item.kind == Item::Kind::Assistant && !item.text.empty()) {
            request.push_back({"assistant", item.text});
        }
    }

    // fold @mention definitions into the last user message. the legacy system
    // prompt already carries the full schema, so skip the schema block here
    rebuildMentionIndex();
    sentSchemaContext_ = true;
    const json blocks = buildPromptBlocks(text);
    std::string mentionContext;
    for (const auto& block : blocks) {
        if (block.value("type", "") == "resource") {
            mentionContext += block["resource"].value("text", "") + "\n";
        }
    }
    if (!mentionContext.empty() && !request.empty()) {
        request.back().content += "\n\n[Referenced objects]\n" + mentionContext;
    }

    apiChat_->setDatabaseNode(contextNode());
    const std::string model = API_MODELS[apiModelIndex_].model;
    apiChat_->buildSystemPromptAsync(
        [this, provider, apiKey, model, request](std::string systemPrompt) {
            apiClient_->sendStreaming(provider, apiKey, model, systemPrompt, request);
        });
}

AISidebarPanel::Item* AISidebarPanel::findToolItem(const std::string& toolId) {
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (it->kind == Item::Kind::Tool && it->toolId == toolId) {
            return &*it;
        }
    }
    return nullptr;
}

void AISidebarPanel::pollAcp() {
    // installer finished?
    if (installer_.check()) {
        const auto& res = installer_.lastResult();
        if (res.success && res.exitCode == 0) {
            items_.push_back({.kind = Item::Kind::Info, .text = "Install finished."});
            agentMissing_ = false;
            if (!pendingPromptText_.empty()) {
                sendAcp(std::exchange(pendingPromptText_, {}));
            }
        } else {
            std::string tail = res.output.empty() ? res.errorMessage : res.output;
            if (tail.size() > 1200) {
                tail = "..." + tail.substr(tail.size() - 1200);
            }
            items_.push_back({.kind = Item::Kind::Error, .text = "Install failed:\n" + tail});
        }
        scrollToBottom_ = true;
    }

    if (!acp_) {
        return;
    }

    bool agentGone = false;
    for (auto& ev : acp_->drainEvents()) {
        switch (ev.type) {
        case AcpEvent::Type::SessionReady:
            if (!pendingPromptText_.empty()) {
                acp_->prompt(buildPromptBlocks(std::exchange(pendingPromptText_, {})));
            }
            break;
        case AcpEvent::Type::MessageDelta:
            if (items_.empty() || items_.back().kind != Item::Kind::Assistant) {
                items_.push_back({.kind = Item::Kind::Assistant});
            }
            items_.back().text += ev.text;
            scrollToBottom_ = true;
            break;
        case AcpEvent::Type::ThoughtDelta:
            if (items_.empty() || items_.back().kind != Item::Kind::Thought) {
                items_.push_back({.kind = Item::Kind::Thought});
            }
            items_.back().text += ev.text;
            scrollToBottom_ = true;
            break;
        case AcpEvent::Type::ToolCall:
        case AcpEvent::Type::ToolCallUpdate: {
            Item* item = findToolItem(ev.tool.id);
            if (!item) {
                items_.push_back({.kind = Item::Kind::Tool, .toolId = ev.tool.id});
                item = &items_.back();
            }
            if (!ev.tool.title.empty()) {
                item->toolTitle = ev.tool.title;
            }
            if (!ev.tool.status.empty()) {
                item->toolStatus = ev.tool.status;
            }
            if (!ev.tool.output.empty()) {
                item->toolOutput += ev.tool.output;
            }
            scrollToBottom_ = true;
            break;
        }
        case AcpEvent::Type::Plan: {
            Item* planItem = nullptr;
            for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
                if (it->kind == Item::Kind::Plan) {
                    planItem = &*it;
                    break;
                }
            }
            if (!planItem) {
                items_.push_back({.kind = Item::Kind::Plan});
                planItem = &items_.back();
            }
            planItem->plan = std::move(ev.plan);
            scrollToBottom_ = true;
            break;
        }
        case AcpEvent::Type::Permission: {
            Item item;
            item.kind = Item::Kind::Permission;
            item.permission = std::move(ev.permission);
            items_.push_back(std::move(item));
            scrollToBottom_ = true;
            break;
        }
        case AcpEvent::Type::TurnEnded:
            if (ev.text == "refusal") {
                items_.push_back({.kind = Item::Kind::Info, .text = "The agent declined."});
            } else if (ev.text == "max_tokens" || ev.text == "max_turn_requests") {
                items_.push_back({.kind = Item::Kind::Info, .text = "Turn stopped: " + ev.text});
            }
            break;
        case AcpEvent::Type::Error: {
            std::string text = ev.text;
            if (const AcpAgentDef* def = currentAgentDef();
                def && text.find("uth") != std::string::npos) {
                text += "\n" + def->authHint;
            }
            items_.push_back({.kind = Item::Kind::Error, .text = std::move(text)});
            scrollToBottom_ = true;
            break;
        }
        case AcpEvent::Type::Exited:
            items_.push_back(
                {.kind = Item::Kind::Info, .text = "Agent exited. Next message restarts it."});
            agentGone = true;
            scrollToBottom_ = true;
            break;
        }
    }
    if (agentGone) {
        stopAgent();
        sentSchemaContext_ = false;
    }
}

void AISidebarPanel::pollApi() {
    apiChat_->pollAsyncPrompt();
    if (!apiClient_->isStreaming() && !apiClient_->isDone()) {
        return;
    }
    const std::string deltas = apiClient_->drainDeltas();
    if (!deltas.empty()) {
        if (items_.empty() || items_.back().kind != Item::Kind::Assistant) {
            items_.push_back({.kind = Item::Kind::Assistant});
        }
        items_.back().text += deltas;
        scrollToBottom_ = true;
    }
    if (apiClient_->consumeDone() && !apiClient_->isStreaming()) {
        if (const std::string error = apiClient_->getError(); !error.empty()) {
            items_.push_back({.kind = Item::Kind::Error, .text = error});
        }
        scrollToBottom_ = true;
    }
}

// ---------------------------------------------------------------- rendering

void AISidebarPanel::render() {
    ensureSettingsLoaded();
    syncContext();
    if (isAcpBackend()) {
        pollAcp();
    } else {
        pollApi();
    }

    renderHeader();
    ImGui::Separator();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float inputRowHeight = ImGui::GetTextLineHeight() + Theme::Spacing::M * 2.0f;
    const float controlsRowHeight = ImGui::GetFrameHeight();
    const float inputAreaHeight = Theme::Spacing::M * 2.0f + inputRowHeight + style.ItemSpacing.y +
                                  controlsRowHeight + Theme::Spacing::S;
    const float availHeight = ImGui::GetContentRegionAvail().y - inputAreaHeight;

    const auto& colors = Application::getInstance().getCurrentColors();
    if (ImGui::BeginChild("##ai_side_messages", ImVec2(-1, availHeight), false)) {
        if (agentMissing_ && isAcpBackend()) {
            renderInstallCard();
        }
        if (items_.empty() && !agentMissing_) {
            constexpr const char* hint = "Ask about your database, or type @ to reference a table.";
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 textSize = ImGui::CalcTextSize(hint, nullptr, false, availW * 0.8f);
            ImGui::SetCursorPos(ImVec2((availW - textSize.x) * 0.5f,
                                       (ImGui::GetContentRegionAvail().y - textSize.y) * 0.5f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availW * 0.8f);
            ImGui::TextColored(colors.subtext0, "%s", hint);
            ImGui::PopTextWrapPos();
        } else {
            renderMessages();
        }
        if (scrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom_ = false;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
    renderInputArea();
}

void AISidebarPanel::renderHeader() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const auto& cat = AcpAgents::catalog();
    const int customIndex = static_cast<int>(cat.size());
    const int apiIndex = customIndex + 1;

    const char* currentLabel = backendIndex_ == customIndex ? "Custom agent"
                               : backendIndex_ == apiIndex
                                   ? "API key"
                                   : cat[static_cast<size_t>(backendIndex_)].name.c_str();

    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::XS));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colors.subtext0, ICON_FA_ROBOT);
    ImGui::SameLine(0, Theme::Spacing::S);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::BeginCombo("##ai_backend", currentLabel)) {
        for (int i = 0; i < static_cast<int>(cat.size()); ++i) {
            if (ImGui::Selectable(cat[static_cast<size_t>(i)].name.c_str(), backendIndex_ == i)) {
                switchBackend(i);
            }
        }
        if (ImGui::Selectable("Custom agent", backendIndex_ == customIndex)) {
            switchBackend(customIndex);
        }
        if (ImGui::Selectable("API key", backendIndex_ == apiIndex)) {
            switchBackend(apiIndex);
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const float rightW = ImGui::CalcTextSize(ICON_FA_ROTATE).x + ImGui::CalcTextSize("Clear").x +
                         ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - rightW);
    if (UIUtils::SmallButton(ICON_FA_ROTATE "##ai_new_session")) {
        stopAgent();
        sentSchemaContext_ = false;
        items_.push_back({.kind = Item::Kind::Info, .text = "New session."});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Restart agent session");
    }
    ImGui::SameLine();
    if (UIUtils::SmallButton("Clear##ai_clear")) {
        apiChat_->cancelAsyncPrompt();
        apiClient_->cancel();
        items_.clear();
    }

    if (backendIndex_ == customIndex) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##ai_custom_cmd", "agent command (speaks ACP on stdio)",
                                     customCmdBuf_, sizeof(customCmdBuf_))) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Application::getInstance().getAppState()->setSetting("ai_custom_agent_cmd",
                                                                 customCmdBuf_);
            stopAgent();
        }
    }

    if (backendIndex_ == apiIndex) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        if (ImGui::BeginCombo("##ai_api_model", API_MODELS[apiModelIndex_].label)) {
            for (int i = 0; i < API_MODEL_COUNT; ++i) {
                if (ImGui::Selectable(API_MODELS[i].label, apiModelIndex_ == i)) {
                    apiModelIndex_ = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (UIUtils::SmallButton("Keys...##ai_keys")) {
            AISettingsDialog::instance().show();
        }
    }

    // context row: database node picker + db tools toggle
    const auto nodes = collectNodes();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colors.subtext0, ICON_FA_DATABASE);
    ImGui::SameLine(0, Theme::Spacing::S);
    const std::string ctxLabel =
        nodes.empty()
            ? "no database selected"
            : nodes[static_cast<size_t>(std::min(contextNodeIndex_, (int)nodes.size() - 1))].label;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::BeginCombo("##ai_ctx_node", ctxLabel.c_str())) {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            if (ImGui::Selectable(nodes[static_cast<size_t>(i)].label.c_str(),
                                  contextNodeIndex_ == i)) {
                contextNodeIndex_ = i;
                sentSchemaContext_ = false;
                mentionIndex_.clear();
            }
        }
        ImGui::EndCombo();
    }
    if (isAcpBackend()) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Tools##ai_mcp", &mcpEnabled_)) {
            Application::getInstance().getAppState()->setSetting("ai_mcp_enabled",
                                                                 mcpEnabled_ ? "1" : "0");
            stopAgent(); // takes effect on next session
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Let the agent inspect schema and run read-only queries\n"
                              "against the selected database (via MCP)");
        }
    }
    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::XS));
}

void AISidebarPanel::renderInstallCard() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const AcpAgentDef* def = currentAgentDef();

    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    if (ImGui::BeginChild("##ai_install_card", ImVec2(-1, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
        ImGui::Indent(Theme::Spacing::M);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(colors.peach, "%s", agentMissingReason_.c_str());
        ImGui::PopTextWrapPos();

        if (def) {
            if (installer_.isRunning()) {
                UIUtils::Spinner("##install_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::SameLine(0, Theme::Spacing::S);
                ImGui::TextColored(colors.subtext0, "Installing...");
            } else {
                if (UIUtils::SmallButton(("Install " + def->name).c_str())) {
                    installer_.start(def->installCmd);
                }
                ImGui::SameLine(0, Theme::Spacing::S);
                ImGui::TextColored(colors.subtext0, "runs: %s", def->installCmd.c_str());
            }
        }
        ImGui::Unindent(Theme::Spacing::M);
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
}

void AISidebarPanel::renderMessages() {
    for (size_t i = 0; i < items_.size(); ++i) {
        renderItem(items_[i], i);
    }

    const bool busy = (acp_ && acp_->isTurnActive()) || apiClient_->isStreaming() ||
                      apiChat_->isBuildingPrompt() ||
                      (acp_ && acp_->isRunning() && !acp_->isSessionReady());
    if (busy) {
        ImGui::Spacing();
        UIUtils::Spinner("##ai_side_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        if (acp_ && !acp_->isSessionReady()) {
            ImGui::SameLine();
            ImGui::TextColored(Application::getInstance().getCurrentColors().subtext0,
                               "Starting agent...");
        }
    }
}

void AISidebarPanel::renderItem(Item& item, size_t index) {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushID(static_cast<int>(index));
    if (index > 0) {
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    }
    ImGui::Indent(Theme::Spacing::M);

    switch (item.kind) {
    case Item::Kind::User:
        ImGui::TextColored(colors.subtext0, "You");
        ImGui::TextWrapped("%s", item.text.c_str());
        break;

    case Item::Kind::Assistant:
        ImGui::TextColored(colors.subtext0, "Assistant");
        renderTextWithCodeBlocks(item.text, index);
        break;

    case Item::Kind::Thought: {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        if (ImGui::TreeNodeEx("Thinking...##thought", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            ImGui::TextWrapped("%s", item.text.c_str());
            ImGui::TreePop();
        }
        ImGui::PopStyleColor();
        break;
    }

    case Item::Kind::Tool: {
        const bool running = item.toolStatus == "pending" || item.toolStatus == "in_progress";
        const bool failed = item.toolStatus == "failed";
        if (running) {
            UIUtils::Spinner("##tool_spin", 5.0f, 2, ImGui::GetColorU32(colors.peach));
        } else {
            ImGui::TextColored(failed ? colors.red : colors.green,
                               failed ? ICON_FA_XMARK : ICON_FA_CHECK);
        }
        ImGui::SameLine(0, Theme::Spacing::S);
        const std::string title = item.toolTitle.empty() ? "Tool call" : item.toolTitle;
        if (item.toolOutput.empty()) {
            ImGui::TextColored(colors.subtext0, "%s", title.c_str());
        } else {
            if (ImGui::Selectable(title.c_str(), item.expanded)) {
                item.expanded = !item.expanded;
            }
            if (item.expanded) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
                if (ImGui::BeginChild("##tool_out", ImVec2(-1, 0),
                                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
                    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                    ImGui::TextColored(colors.subtext0, "%s", item.toolOutput.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
        }
        break;
    }

    case Item::Kind::Plan:
        ImGui::TextColored(colors.subtext0, "Plan");
        for (const auto& entry : item.plan) {
            if (entry.status == "completed") {
                ImGui::TextColored(colors.green, ICON_FA_CIRCLE_CHECK);
            } else if (entry.status == "in_progress") {
                ImGui::TextColored(colors.peach, ICON_FA_CIRCLE_HALF_STROKE);
            } else {
                ImGui::TextColored(colors.overlay1, ICON_FA_CIRCLE);
            }
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::TextWrapped("%s", entry.content.c_str());
        }
        break;

    case Item::Kind::Permission: {
        ImGui::TextColored(colors.peach, ICON_FA_TRIANGLE_EXCLAMATION " Permission");
        ImGui::TextWrapped("%s", item.permission.title.c_str());
        if (item.permissionAnswered) {
            ImGui::TextColored(colors.subtext0, "-> %s", item.permissionChoice.c_str());
        } else if (acp_) {
            for (size_t i = 0; i < item.permission.options.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine(0, Theme::Spacing::S);
                }
                const auto& opt = item.permission.options[i];
                const bool allow = opt.kind.starts_with("allow");
                if (UIUtils::SmallButton(std::format("{}##perm{}", opt.name, i).c_str(),
                                         allow ? UIUtils::ButtonVariant::Primary
                                               : UIUtils::ButtonVariant::Danger)) {
                    acp_->respondPermission(item.permission.rpcId, opt.optionId);
                    item.permissionAnswered = true;
                    item.permissionChoice = opt.name;
                }
            }
        }
        break;
    }

    case Item::Kind::Info:
        ImGui::TextColored(colors.subtext0, "%s", item.text.c_str());
        break;

    case Item::Kind::Error:
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(colors.red, "%s", item.text.c_str());
        ImGui::PopTextWrapPos();
        break;
    }

    ImGui::Unindent(Theme::Spacing::M);
    ImGui::PopID();
}

void AISidebarPanel::renderTextWithCodeBlocks(const std::string& content, size_t index) {
    MarkdownText::render(content, std::format("side_{}", index));
}

// ---------------------------------------------------------------- input + mentions

namespace {
    int sidebarInputCallbackThunk(ImGuiInputTextCallbackData* data);
}

int aiSidebarInputCallback(void* dataPtr) {
    auto* data = static_cast<ImGuiInputTextCallbackData*>(dataPtr);
    auto* self = static_cast<AISidebarPanel*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (self->pendingReplaceStart_ >= 0 && self->pendingReplaceEnd_ <= data->BufTextLen) {
            data->DeleteChars(self->pendingReplaceStart_,
                              self->pendingReplaceEnd_ - self->pendingReplaceStart_);
            data->InsertChars(self->pendingReplaceStart_, self->pendingInsertText_.c_str());
            data->CursorPos =
                self->pendingReplaceStart_ + static_cast<int>(self->pendingInsertText_.size());
            self->pendingReplaceStart_ = -1;
            self->pendingReplaceEnd_ = -1;
            self->pendingInsertText_.clear();
        }
        self->cursorPos_ = data->CursorPos;
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (self->mentionOpen_) {
            self->mentionNavDelta_ += (data->EventKey == ImGuiKey_UpArrow) ? -1 : 1;
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        if (self->mentionOpen_) {
            self->mentionAcceptRequested_ = true;
        }
    }
    return 0;
}

namespace {
    int sidebarInputCallbackThunk(ImGuiInputTextCallbackData* data) {
        return aiSidebarInputCallback(data);
    }
} // namespace

void AISidebarPanel::updateMentionState() {
    const std::string text(inputBuf_);
    const int cursor = std::min(cursorPos_, static_cast<int>(text.size()));

    int at = -1;
    for (int i = cursor - 1; i >= 0; --i) {
        const char c = text[static_cast<size_t>(i)];
        if (c == '@') {
            at = i;
            break;
        }
        if (!isWordChar(c)) {
            break;
        }
    }
    // '@' must start a word
    if (at > 0 && isWordChar(text[static_cast<size_t>(at - 1)])) {
        at = -1;
    }

    if (at < 0 || at == mentionDismissedStart_) {
        mentionOpen_ = false;
        return;
    }
    if (at != mentionStart_) {
        mentionSel_ = 0;
        mentionDismissedStart_ = -1;
        if (mentionIndex_.empty()) {
            rebuildMentionIndex();
        }
    }
    mentionStart_ = at;
    mentionFilter_ = text.substr(static_cast<size_t>(at) + 1, static_cast<size_t>(cursor - at - 1));

    // case-insensitive substring filter
    std::string needle = mentionFilter_;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    mentionMatches_.clear();
    for (int i = 0; i < static_cast<int>(mentionIndex_.size()); ++i) {
        std::string hay = mentionIndex_[static_cast<size_t>(i)].name;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (needle.empty() || hay.find(needle) != std::string::npos) {
            mentionMatches_.push_back(i);
        }
    }
    mentionOpen_ = !mentionMatches_.empty();
    if (mentionSel_ >= static_cast<int>(mentionMatches_.size())) {
        mentionSel_ = 0;
    }
}

void AISidebarPanel::acceptMention(const MentionEntry& entry) {
    pendingReplaceStart_ = mentionStart_;
    pendingReplaceEnd_ = std::min(cursorPos_, static_cast<int>(std::strlen(inputBuf_)));
    pendingInsertText_ = "@" + entry.name + " ";
    mentionOpen_ = false;
    mentionDismissedStart_ = -1;
    focusInput_ = true;
}

void AISidebarPanel::renderMentionPopupAt(float x, float y, float width) {
    if (!mentionOpen_ || mentionMatches_.empty()) {
        return;
    }
    const auto& colors = Application::getInstance().getCurrentColors();

    if (mentionNavDelta_ != 0) {
        const int count = static_cast<int>(mentionMatches_.size());
        mentionSel_ = ((mentionSel_ + mentionNavDelta_) % count + count) % count;
        mentionNavDelta_ = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        mentionOpen_ = false;
        mentionDismissedStart_ = mentionStart_;
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.surface0);
    if (ImGui::Begin("##ai_mention_popup", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        const int shown = std::min(static_cast<int>(mentionMatches_.size()), MENTION_MAX_VISIBLE);
        for (int i = 0; i < shown; ++i) {
            const auto& entry = mentionIndex_[static_cast<size_t>(mentionMatches_[i])];
            if (ImGui::Selectable(entry.label.c_str(), i == mentionSel_)) {
                acceptMention(entry);
            }
        }
        if (static_cast<int>(mentionMatches_.size()) > shown) {
            ImGui::TextColored(colors.subtext0, "  ... %d more",
                               static_cast<int>(mentionMatches_.size()) - shown);
        }
        if (mentionAcceptRequested_) {
            mentionAcceptRequested_ = false;
            acceptMention(mentionIndex_[static_cast<size_t>(mentionMatches_[mentionSel_])]);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void AISidebarPanel::renderInputArea() {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::M));

    const float inputRowHeight = ImGui::GetTextLineHeight() + Theme::Spacing::M * 2.0f;
    const float containerHeight = Theme::Spacing::M * 2.0f + inputRowHeight +
                                  ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();

    ImVec2 inputRectMin{};
    float inputWidth = 0.0f;

    if (ImGui::BeginChild("##ai_side_input", ImVec2(-1, containerHeight),
                          ImGuiChildFlags_Borders)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushItemWidth(-1);

        if (focusInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInput_ = false;
        }

        const bool submitted = ImGui::InputTextWithHint(
            "##ai_side_text", "Ask about your database... (@ mentions a table)", inputBuf_,
            sizeof(inputBuf_),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways |
                ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion,
            sidebarInputCallbackThunk, this);
        inputRectMin = ImGui::GetItemRectMin();
        inputWidth = ImGui::GetItemRectSize().x;

        ImGui::PopItemWidth();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        updateMentionState();

        if (submitted) {
            if (mentionOpen_ && !mentionMatches_.empty()) {
                acceptMention(mentionIndex_[static_cast<size_t>(mentionMatches_[mentionSel_])]);
                focusInput_ = true;
            } else {
                sendMessage();
            }
        }

        // controls row
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, ICON_FA_WAND_MAGIC_SPARKLES);

        const float sendBtnWidth = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - sendBtnWidth);

        const bool acpBusy = acp_ && (acp_->isTurnActive() || !pendingPromptText_.empty());
        const bool apiBusy = apiClient_->isStreaming() || apiChat_->isBuildingPrompt();
        if (acpBusy || apiBusy) {
            if (UIUtils::IconButton(ICON_FA_STOP, UIUtils::ButtonVariant::Danger,
                                    ImVec2(sendBtnWidth, 0))) {
                if (acp_) {
                    acp_->cancelTurn();
                    pendingPromptText_.clear();
                }
                apiChat_->cancelAsyncPrompt();
                apiClient_->cancel();
            }
        } else {
            if (UIUtils::IconButton(ICON_FA_ARROW_UP, UIUtils::ButtonVariant::Primary,
                                    ImVec2(sendBtnWidth, 0))) {
                sendMessage();
            }
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    renderMentionPopupAt(inputRectMin.x, inputRectMin.y - Theme::Spacing::S, inputWidth);
}
