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
#include "imgui_internal.h"
#include "themes.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/markdown_text.hpp"
#include "utils/app_paths.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <acp/log.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <iomanip>
#include <spdlog/spdlog.h>
#include <sstream>

using json = nlohmann::json;

namespace {
    struct ApiModelOption {
        const char* label;
        const char* model;
        AIProvider provider;
    };
    constexpr ApiModelOption API_MODELS[] = {
        {"claude-opus-5", "claude-opus-5", AIProvider::ANTHROPIC},
        {"claude-sonnet-5", "claude-sonnet-5", AIProvider::ANTHROPIC},
        {"claude-haiku-4-5", "claude-haiku-4-5", AIProvider::ANTHROPIC},
        {"gpt-5.6", "gpt-5.6", AIProvider::OPENAI},
        {"gpt-5.6-terra", "gpt-5.6-terra", AIProvider::OPENAI},
        {"gpt-5.6-luna", "gpt-5.6-luna", AIProvider::OPENAI},
        {"gemini-3.8-flash", "gemini-3.8-flash", AIProvider::GEMINI},
        {"gemini-3.1-pro", "gemini-3.1-pro-preview", AIProvider::GEMINI},
    };
    constexpr int API_MODEL_COUNT = sizeof(API_MODELS) / sizeof(API_MODELS[0]);
    constexpr int MENTION_MAX_VISIBLE = 8;
    // schema loads the picker may kick off per rebuild. bounded because a server can
    // hold dozens of databases and each load opens a connection to one of them --
    // typing '@' must not fan out to all of them at once
    constexpr int CONTEXT_AUTOLOAD_MAX_NODES = 8;
    constexpr double CONTEXT_REBUILD_INTERVAL = 0.25; // seconds

    struct SlashCommand {
        const char* name;
        const char* help;
    };
    constexpr SlashCommand CLIENT_COMMANDS[] = {
        {"new", "Start a fresh agent session"},
        {"clear", "Clear the conversation"},
    };
    constexpr int CLIENT_COMMAND_COUNT = sizeof(CLIENT_COMMANDS) / sizeof(CLIENT_COMMANDS[0]);

    // input box metrics, shared by the height calc and the renderer.
    // the container pads all four sides, the frame padding insets the text on top of
    // that -- kept small at the bottom so the controls row sits close to the border
    constexpr float INPUT_CONTAINER_PAD_X = Theme::Spacing::M;
    constexpr float INPUT_CONTAINER_PAD_Y = Theme::Spacing::S;
    constexpr float INPUT_FRAME_PAD_X = Theme::Spacing::M;
    constexpr float INPUT_FRAME_PAD_Y = Theme::Spacing::M;
    constexpr float INPUT_ROW_GAP = Theme::Spacing::S;
    constexpr float INPUT_BOTTOM_MARGIN = Theme::Spacing::M;
    constexpr float INPUT_TEXT_SLACK = 2.0f;
    constexpr float INPUT_ROUNDING = 10.0f;
    constexpr float INPUT_MAX_LINES = 8.0f;
    constexpr const char* INPUT_HINT = "Ask about your database";

    // height of the icon row under the text. deliberately independent of
    // style.FramePadding: the renderer pushes its own padding, so GetFrameHeight() there
    // disagrees with the value the height calc sees, leaving the text field a few pixels
    // short -- which shows a scrollbar and makes the text drift while typing.
    float inputControlsHeight() {
        return ImGui::GetTextLineHeight() + Theme::Spacing::S * 2.0f;
    }

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

    // Working directory handed to the agent. Deliberately a scratch dir of our own:
    // agents carry their own file and shell tools that do not go through ACP, so a
    // cwd of $HOME would point them at everything and pull in stray project config.
    std::string agentWorkingDir() {
        const std::filesystem::path dir = AppPaths::ensureSubdir("agent");
        if (dir.empty()) {
            spdlog::warn("could not create the agent working directory");
            return AppPaths::dataDir().string();
        }
        return dir.string();
    }

    bool isWordChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
    }

    constexpr float CHIP_PAD_X = Theme::Spacing::M;
    constexpr float CHIP_PAD_Y = Theme::Spacing::S;

    float chipHeight() {
        return ImGui::GetTextLineHeight() + CHIP_PAD_Y * 2.0f;
    }

    float chipWidth(const std::string& label) {
        // text, the close glyph, and padding on both ends
        return ImGui::CalcTextSize(label.c_str()).x + CHIP_PAD_X * 2.0f +
               ImGui::GetTextLineHeight() + Theme::Spacing::S;
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

const char* AISidebarPanel::contextKindIcon(ContextItem::Kind kind) {
    switch (kind) {
    case ContextItem::Kind::Database:
        return ICON_FA_DATABASE;
    case ContextItem::Kind::View:
        return ICON_FA_EYE;
    case ContextItem::Kind::Sequence:
        return ICON_FA_LIST_OL;
    case ContextItem::Kind::Table:
        break;
    }
    return ICON_FA_TABLE;
}

AISidebarPanel::AISidebarPanel()
    : apiClient_(std::make_unique<AIClient>()), apiChat_(std::make_unique<AIChatState>(nullptr)) {
    // point acp-cpp at our data dir and log sink, once
    static const bool configured = [] {
        acp::registry::setInstallRoot(AppPaths::dataDir() / "agents");
        acp::setLogger([](acp::LogLevel level, const std::string& msg) {
            switch (level) {
            case acp::LogLevel::Debug:
                spdlog::debug("ACP: {}", msg);
                break;
            case acp::LogLevel::Info:
                spdlog::info("ACP: {}", msg);
                break;
            case acp::LogLevel::Warn:
                spdlog::warn("ACP: {}", msg);
                break;
            case acp::LogLevel::Error:
                spdlog::error("ACP: {}", msg);
                break;
            }
        });
        return true;
    }();
    (void)configured;
}

AISidebarPanel::~AISidebarPanel() {
    saveCurrentSession();
}

// ---------------------------------------------------------------- backends

bool AISidebarPanel::isAcpBackend() const {
    return backendIndex_ <= static_cast<int>(agentDefs_.size());
}

const AcpAgentDef* AISidebarPanel::currentAgentDef() const {
    const auto& cat = agentDefs_;
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
        std::string tried = def->runCmd.empty() ? "" : "`" + def->runCmd.front() + "`";
        for (const auto& runner : AcpAgents::runners()) {
            const bool usable = runner.python ? !def->pyPackage.empty() : !def->npmPackage.empty();
            if (usable) {
                tried += (tried.empty() ? "" : ", ") + ("`" + runner.tool + "`");
            }
        }
        missingReason =
            std::format("{} is not installed (looked for {} on your PATH).", def->name, tried);
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
    agentDefs_ = AcpAgents::availableAgents();
    selectBackend(appState->getSetting("ai_sidebar_backend", agentDefs_.front().id));
    const std::string custom = appState->getSetting("ai_custom_agent_cmd", "");
    std::strncpy(customCmdBuf_, custom.c_str(), sizeof(customCmdBuf_) - 1);
    mcpEnabled_ = appState->getSetting("ai_mcp_enabled", "1") == "1";
    settingsLoaded_ = true;
}

void AISidebarPanel::switchBackend(int newIndex) {
    if (newIndex == backendIndex_) {
        return;
    }
    // sessions are per backend
    saveCurrentSession();
    items_.clear();
    selectedContext_.clear();
    currentSessionId_ = 0;
    resumeSessionId_.clear();
    backendIndex_ = newIndex;
    stopAgent();
    agentWarmupDone_ = false;
    agentMissing_ = false;
    agentMissingReason_.clear();
    sentSchemaContext_ = false;
    Application::getInstance().getAppState()->setSetting("ai_sidebar_backend", backendId());
}

std::string AISidebarPanel::backendId() const {
    if (backendIndex_ >= 0 && backendIndex_ < static_cast<int>(agentDefs_.size())) {
        return agentDefs_[static_cast<size_t>(backendIndex_)].id;
    }
    return backendIndex_ == static_cast<int>(agentDefs_.size()) ? "custom" : "api";
}

void AISidebarPanel::selectBackend(const std::string& id) {
    backendIndex_ = 0;
    for (size_t i = 0; i < agentDefs_.size(); ++i) {
        if (agentDefs_[i].id == id) {
            backendIndex_ = static_cast<int>(i);
        }
    }
    if (id == "custom") {
        backendIndex_ = static_cast<int>(agentDefs_.size());
    } else if (id == "api") {
        backendIndex_ = static_cast<int>(agentDefs_.size()) + 1;
    }
}

void AISidebarPanel::stopAgent() {
    acp_.reset();
    agentCommands_.clear();
    // nothing is left to serve, so close the port rather than leave it listening
    mcp_.stop();
    pendingPromptBlocks_ = json::array();
}

bool AISidebarPanel::isBusy() const {
    if (acp_ && (acp_->isTurnActive() || !pendingPromptBlocks_.empty())) {
        return true;
    }
    return apiClient_->isStreaming() || apiChat_->isBuildingPrompt();
}

// ---------------------------------------------------------------- sessions

namespace {
    // "5m", "2h", "3d" from sqlite's utc CURRENT_TIMESTAMP
    std::string relativeAge(const std::string& stamp) {
        std::tm tm{};
        std::istringstream in(stamp);
        in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (in.fail()) {
            return "";
        }
#ifdef _WIN32
        const std::time_t then = _mkgmtime(&tm);
#else
        const std::time_t then = timegm(&tm);
#endif
        const auto secs = static_cast<long>(std::time(nullptr) - then);
        if (secs < 60) {
            return "now";
        }
        if (secs < 3600) {
            return std::to_string(secs / 60) + "m";
        }
        if (secs < 86400) {
            return std::to_string(secs / 3600) + "h";
        }
        return std::to_string(secs / 86400) + "d";
    }
} // namespace

void AISidebarPanel::saveCurrentSession() {
    const auto first = std::find_if(items_.begin(), items_.end(),
                                    [](const Item& item) { return item.kind == Item::Kind::User; });
    if (first == items_.end()) {
        return;
    }
    std::string title = first->text.substr(0, first->text.find('\n'));
    if (title.size() > 80) {
        title = title.substr(0, 80) + "…";
    }

    std::string acpSessionId;
    std::string transcript;
    if (isAcpBackend()) {
        acpSessionId = acp_ ? acp_->sessionId() : "";
        if (acpSessionId.empty()) {
            return; // nothing to resume yet
        }
    } else {
        // only text kinds occur on the api backend
        json rows = json::array();
        for (const auto& item : items_) {
            rows.push_back({{"kind", static_cast<int>(item.kind)},
                            {"text", item.text},
                            {"note", item.contextNote}});
        }
        transcript = rows.dump();
    }
    auto* appState = Application::getInstance().getAppState();
    const int id =
        appState->saveAiSession(currentSessionId_, backendId(), acpSessionId, title, transcript);
    if (id > 0) {
        currentSessionId_ = id;
    }
}

void AISidebarPanel::startNewSession() {
    saveCurrentSession();
    items_.clear();
    selectedContext_.clear();
    currentSessionId_ = 0;
    resumeSessionId_.clear();
    sentSchemaContext_ = false;
    if (isAcpBackend()) {
        stopAgent();
        agentWarmupDone_ = false; // bring the replacement session straight back up
    } else {
        apiChat_->cancelAsyncPrompt();
        apiClient_->cancel();
    }
    focusInput_ = true;
}

void AISidebarPanel::openSession(const AiSession& session) {
    if (session.id == currentSessionId_) {
        return;
    }
    saveCurrentSession();
    items_.clear();
    selectedContext_.clear();
    currentSessionId_ = session.id;
    if (isAcpBackend()) {
        stopAgent();
        resumeSessionId_ = session.acpSessionId;
        agentWarmupDone_ = false;  // render() restarts the agent, which replays the history
        sentSchemaContext_ = true; // the loaded session already had it
    } else {
        apiChat_->cancelAsyncPrompt();
        apiClient_->cancel();
        auto* appState = Application::getInstance().getAppState();
        const json rows = json::parse(appState->getAiSessionTranscript(session.id), nullptr, false);
        for (const auto& row : rows.is_array() ? rows : json::array()) {
            items_.push_back({.kind = static_cast<Item::Kind>(row.value("kind", 0)),
                              .text = row.value("text", ""),
                              .contextNote = row.value("note", "")});
        }
    }
    scrollToBottom_ = true;
    focusInput_ = true;
}

void AISidebarPanel::renderSessionPopup() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
    if (!ImGui::BeginPopup("##ai_session_popup")) { // padding comes from the theme
        return;
    }
    if (sessionRows_.empty()) {
        ImGui::TextDisabled("No earlier sessions");
    }
    int deleteId = 0;
    for (const auto& row : sessionRows_) {
        ImGui::PushID(row.id);
        const std::string age = relativeAge(row.updatedAt);
        const float ageW = ImGui::CalcTextSize(age.c_str()).x;
        if (ImGui::Selectable("##row", row.id == currentSessionId_)) {
            openSession(row);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem("Delete")) {
                deleteId = row.id;
            }
            ImGui::EndPopup();
        }
        // title left, age right, painted over the selectable so the cursor never moves
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float textY = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, ImVec2(max.x - ageW - Theme::Spacing::L, max.y), true);
        draw->AddText(ImVec2(min.x + Theme::Spacing::S, textY), ImGui::GetColorU32(ImGuiCol_Text),
                      row.title.c_str());
        draw->PopClipRect();
        draw->AddText(ImVec2(max.x - ageW - Theme::Spacing::S, textY),
                      ImGui::GetColorU32(colors.subtext0), age.c_str());
        ImGui::PopID();
    }
    if (deleteId > 0) {
        Application::getInstance().getAppState()->deleteAiSession(deleteId);
        std::erase_if(sessionRows_, [deleteId](const AiSession& s) { return s.id == deleteId; });
        if (deleteId == currentSessionId_) {
            currentSessionId_ = 0; // the next save creates a fresh row
        }
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- context

std::vector<AISidebarPanel::NodeRef> AISidebarPanel::collectNodes() const {
    std::vector<NodeRef> nodes;

    for (const auto& db : Application::getInstance().getDatabases()) {
        if (!db || !db->isConnected()) {
            continue;
        }
        const auto type = db->getConnectionInfo().type;
        const std::string conn = db->getConnectionInfo().name;

        if (isFileDatabase(type)) {
            if (auto* fileDb = dynamic_cast<FileDatabase*>(db.get())) {
                nodes.push_back({conn, fileDb});
            }
            continue;
        }

        auto collect = [&nodes, &conn]<typename T>(T* server) {
            if (!server) {
                return;
            }
            for (auto& [name, node] : server->getDatabaseDataMap()) {
                if constexpr (std::is_same_v<T, PostgresDatabase>) {
                    if (node->schemasLoaded && !node->schemas.empty()) {
                        for (const auto& schema : node->schemas) {
                            nodes.push_back(
                                {conn + "." + name + "." + schema->getName(), schema.get()});
                        }
                        continue;
                    }
                }
                nodes.push_back({conn + "." + name, node.get()});
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
    }

    std::sort(nodes.begin(), nodes.end(),
              [](const NodeRef& a, const NodeRef& b) { return a.label < b.label; });
    return nodes;
}

IDatabaseNode* AISidebarPanel::contextNode() {
    // whatever the user pinned wins; otherwise fall back to the first connected node
    for (const auto& item : selectedContext_) {
        if (item.node) {
            return item.node;
        }
    }
    const auto nodes = collectNodes();
    return nodes.empty() ? nullptr : nodes.front().node;
}

void AISidebarPanel::syncContext() {
    if (auto db = Application::getInstance().getSelectedDatabase(); db.get() != lastDb_) {
        lastDb_ = db.get();
        sentSchemaContext_ = false;
        contextCandidates_.clear();
    }
    const auto nodes = collectNodes();
    // the sidebar only polls these while the Databases tab is rendering, so an async
    // schema load started for the picker would otherwise never complete here
    for (const auto& ref : nodes) {
        if (ref.node) {
            ref.node->checkLoadingStatus();
        }
    }
    // drop pinned context whose node went away with a disconnect
    if (!selectedContext_.empty()) {
        std::erase_if(selectedContext_, [&nodes](const ContextItem& item) {
            // entries for closed connections carry no node; keep those
            return item.node != nullptr &&
                   std::none_of(nodes.begin(), nodes.end(),
                                [&item](const NodeRef& ref) { return ref.node == item.node; });
        });
    }
    // keep the mcp tool pointed at the current node
    IDatabaseNode* node = contextNode();
    mcp_.setNode(node, node ? node->getName() : "");

    // publish every saved connection so the agent can see, and open, closed ones
    std::vector<McpConnectionInfo> connections;
    for (const auto& db : Application::getInstance().getDatabases()) {
        if (!db) {
            continue;
        }
        const auto info = db->getConnectionInfo();
        connections.push_back({info.name, dbTypeLabel(info.type), db->isConnected()});
    }
    mcp_.setConnections(std::move(connections));

    serviceAgentConnectRequests();
}

void AISidebarPanel::serviceAgentConnectRequests() {
    // the tool handler blocks on the http thread; opening a connection has to happen
    // here, on the ui thread, and report back when the async connect settles
    if (pendingConnectDb_) {
        pendingConnectDb_->checkConnectionStatusAsync();
        const auto info = pendingConnectDb_->getConnectionInfo();
        if (pendingConnectDb_->isConnected()) {
            mcp_.finishConnectRequest(true, std::format("Connected to {}.", info.name));
            pendingConnectDb_.reset();
        } else if (!pendingConnectDb_->isConnecting() &&
                   pendingConnectDb_->hasAttemptedConnection()) {
            const std::string error = pendingConnectDb_->getLastConnectionError();
            mcp_.finishConnectRequest(false, std::format("Could not connect to {}: {}", info.name,
                                                         error.empty() ? "unknown error" : error));
            pendingConnectDb_.reset();
        }
        return;
    }

    const auto request = mcp_.takeConnectRequest();
    if (!request) {
        return;
    }

    std::shared_ptr<DatabaseInterface> target;
    for (const auto& db : Application::getInstance().getDatabases()) {
        if (db && db->getConnectionInfo().name == *request) {
            target = db;
            break;
        }
    }
    if (!target) {
        mcp_.finishConnectRequest(false, std::format("No connection named '{}'.", *request));
        return;
    }
    if (target->isConnected()) {
        mcp_.finishConnectRequest(true, std::format("{} is already connected.", *request));
        return;
    }

    target->startConnectionAsync();
    pendingConnectDb_ = target;
    items_.push_back(
        {.kind = Item::Kind::Info, .text = std::format("Agent is connecting to {}...", *request)});
    scrollToBottom_ = true;
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

void AISidebarPanel::rebuildContextCandidates() {
    contextCandidates_.clear();
    int loadBudget = CONTEXT_AUTOLOAD_MAX_NODES;

    // closed connections are offered too: the agent can open one with connect_database
    for (const auto& db : Application::getInstance().getDatabases()) {
        if (db && !db->isConnected()) {
            contextCandidates_.push_back({ContextItem::Kind::Database, db->getConnectionInfo().name,
                                          "not connected", nullptr});
        }
    }

    for (const auto& ref : collectNodes()) {
        if (!ref.node) {
            continue;
        }
        contextCandidates_.push_back({ContextItem::Kind::Database, ref.label, "", ref.node});

        // schema is loaded lazily on sidebar expand; the picker needs it too, so pull
        // it in for nodes the user has not opened yet
        if (loadBudget > 0 && !ref.node->isTablesLoaded() && !ref.node->isLoadingTables()) {
            ref.node->startTablesLoadAsync(false);
            --loadBudget;
        }
        if (loadBudget > 0 && !ref.node->isViewsLoaded() && !ref.node->isLoadingViews()) {
            ref.node->startViewsLoadAsync(false);
            --loadBudget;
        }

        if (ref.node->isTablesLoaded()) {
            for (const auto& table : ref.node->getTables()) {
                contextCandidates_.push_back(
                    {ContextItem::Kind::Table, table.name, ref.label, ref.node});
            }
        }
        if (ref.node->isViewsLoaded()) {
            for (const auto& view : ref.node->getViews()) {
                contextCandidates_.push_back(
                    {ContextItem::Kind::View, view.name, ref.label, ref.node});
            }
        }
        for (const auto& seq : ref.node->getSequences()) {
            contextCandidates_.push_back({ContextItem::Kind::Sequence, seq, ref.label, ref.node});
        }
    }
}

std::string AISidebarPanel::contextItemText(const ContextItem& item) const {
    if (!item.node) {
        // a saved connection that is not open yet; point the agent at the tool for it
        return std::format("Database \"{}\" is saved in DearSQL but not connected yet. "
                           "Open it with the connect_database tool (name: \"{}\"), then "
                           "inspect it with list_tables and run_query.\n",
                           item.name, item.name);
    }
    switch (item.kind) {
    case ContextItem::Kind::Database:
        return std::format("Database {} ({})\n{}", item.name,
                           dbTypeLabel(item.node->getDatabaseType()), schemaOverview(item.node));
    case ContextItem::Kind::Table:
    case ContextItem::Kind::View: {
        const bool isView = item.kind == ContextItem::Kind::View;
        const auto& list = isView ? item.node->getViews() : item.node->getTables();
        for (const auto& table : list) {
            if (table.name == item.name) {
                return tableDdlText(table, isView);
            }
        }
        return std::format("{} {}\n", isView ? "View" : "Table", item.name);
    }
    case ContextItem::Kind::Sequence:
        return std::format("Sequence {} in {}\n", item.name, item.owner);
    }
    return "";
}

json AISidebarPanel::buildPromptBlocks(const std::string& text) {
    json blocks = json::array();

    if (!sentSchemaContext_) {
        IDatabaseNode* node = contextNode();
        std::string intro = "You are helping with a ";
        if (node) {
            intro += std::string(dbTypeLabel(node->getDatabaseType())) + " database";
        } else {
            intro += "database (none connected yet)";
        }
        intro += " inside DearSQL, a desktop SQL client. Prefer answering with SQL in "
                 "```sql code blocks. Do not modify files. If the DearSQL MCP tools are "
                 "available, use them: list_databases shows the saved connections, "
                 "connect_database opens one that is not connected yet, and list_tables "
                 "and run_query inspect the open database. Connect on your own when you "
                 "need a database that is not open.";
        blocks.push_back({{"type", "text"}, {"text", intro}});
        // only dump the whole schema when the user has not pinned anything specific
        if (node && selectedContext_.empty()) {
            blocks.push_back({{"type", "resource"},
                              {"resource",
                               {{"uri", "dearsql://schema"},
                                {"mimeType", "text/plain"},
                                {"text", schemaOverview(node)}}}});
        }
        sentSchemaContext_ = true;
    }

    blocks.push_back({{"type", "text"}, {"text", text}});

    for (const auto& item : selectedContext_) {
        const std::string body = contextItemText(item);
        if (body.empty()) {
            continue;
        }
        blocks.push_back({{"type", "resource"},
                          {"resource",
                           {{"uri", "dearsql://context/" + item.name},
                            {"mimeType", "text/plain"},
                            {"text", body}}}});
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
    // one prompt in flight; a second would overwrite the queued blocks
    if (isBusy()) {
        return;
    }
    text = text.substr(start, text.find_last_not_of(" \t\n\r") - start + 1);

    std::string note;
    for (const auto& item : selectedContext_) {
        note += (note.empty() ? "" : ", ") + item.name;
    }
    items_.push_back({.kind = Item::Kind::User,
                      .text = text,
                      .contextNote = note.empty() ? "" : "context: " + note});
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
    // the context rode along with this message; start the next one clean
    selectedContext_.clear();
}

void AISidebarPanel::sendAcp(const std::string& text) {
    // resolve the pinned context into blocks NOW. the chips are cleared as soon as the
    // message is queued, so deferring this until the session is ready would send the
    // message with its context already gone -- which is every first message, since the
    // agent is still starting up then.
    queueOrSendAcp(buildPromptBlocks(text));
}

bool AISidebarPanel::ensureAgentStarted() {
    if (acp_ && acp_->isRunning()) {
        return true;
    }

    std::string reason;
    const auto argv = currentInvocation(reason);
    if (argv.empty()) {
        agentMissing_ = true;
        agentMissingReason_ = reason;
        return false;
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
    auto [ok, err] = acp_->start(argv, agentWorkingDir(), mcpUrl, "dearsql",
                                 mcpUrl.empty() ? std::string{} : mcp_.token(),
                                 std::exchange(resumeSessionId_, std::string{}));
    if (!ok) {
        items_.push_back({.kind = Item::Kind::Error, .text = err});
        acp_.reset();
        return false;
    }
    return true;
}

void AISidebarPanel::queueOrSendAcp(json blocks) {
    if (acp_ && acp_->isSessionReady()) {
        acp_->prompt(blocks);
        return;
    }
    if (acp_ && acp_->isRunning()) {
        pendingPromptBlocks_ = std::move(blocks); // session still being created
        return;
    }
    if (ensureAgentStarted() || agentMissing_) {
        // hold the message until the session is ready, or until the agent is installed
        pendingPromptBlocks_ = std::move(blocks);
    }
}

void AISidebarPanel::sendApi(const std::string& text) {
    auto* appState = Application::getInstance().getAppState();
    const AIProvider provider = API_MODELS[apiModelIndex_].provider;
    std::string apiKey = appState->getSetting(apiKeySettingFor(provider), "");
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

    // fold the pinned context into the last user message. the legacy system prompt
    // already carries the full schema, so skip the schema block here
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
        if (res.ok && res.exitCode == 0) {
            items_.push_back({.kind = Item::Kind::Info, .text = "Install finished."});
            agentMissing_ = false;
            if (!pendingPromptBlocks_.empty()) {
                queueOrSendAcp(std::exchange(pendingPromptBlocks_, json::array()));
            }
        } else {
            std::string tail = res.output.empty() ? res.error : res.output;
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
            if (!pendingPromptBlocks_.empty()) {
                acp_->prompt(std::exchange(pendingPromptBlocks_, json::array()));
            }
            break;
        case AcpEvent::Type::MessageDelta:
            if (items_.empty() || items_.back().kind != Item::Kind::Assistant) {
                items_.push_back({.kind = Item::Kind::Assistant});
            }
            items_.back().text += ev.text;
            scrollToBottom_ = true;
            break;
        case AcpEvent::Type::UserMessageDelta:
            if (items_.empty() || items_.back().kind != Item::Kind::User) {
                items_.push_back({.kind = Item::Kind::User});
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
        case AcpEvent::Type::Commands:
            agentCommands_ = std::move(ev.commands);
            break;
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

void AISidebarPanel::tick() {
    ensureSettingsLoaded();

    const bool settingsOpen = AISettingsDialog::instance().isOpen();
    if (settingsDialogWasOpen_ && !settingsOpen) {
        const bool enabled =
            Application::getInstance().getAppState()->getSetting("ai_mcp_enabled", "1") == "1";
        if (enabled != mcpEnabled_) {
            mcpEnabled_ = enabled;
            stopAgent();
        }
    }
    settingsDialogWasOpen_ = settingsOpen;

    syncContext();
    if (isAcpBackend()) {
        pollAcp();
    } else {
        pollApi();
    }

    const bool busy = isBusy();
    if (wasBusy_ && !busy) {
        saveCurrentSession(); // a turn just finished
    }
    wasBusy_ = busy;
}

void AISidebarPanel::render() {
    ensureSettingsLoaded(); // first render lands before the first tick
    // warm up only once the tab has actually been shown
    if (!agentWarmupDone_ && isAcpBackend()) {
        agentWarmupDone_ = true;
        ensureAgentStarted();
    }

    renderHeader();
    ImGui::Separator();

    // the message list owns all the scrolling; never let it collapse to a negative
    // size, which imgui would read as "fill minus n" and overflow the tab.
    // the footer is the spacing imgui adds after the list, the input box itself, and a
    // margin below it -- under-reserving here is what clipped the input's bottom edge.
    // the input box and the History button in the tab strip are both anchored to the
    // same bottom edge, so their top edges line up exactly when their heights match.
    // the anchor is the History button's height; without one, fall back to a margin.
    const float inputH = computeInputHeight();
    const float bottomMargin = inputBottomAnchor_ > 0.0f
                                   ? std::max(0.0f, inputBottomAnchor_ - inputH)
                                   : INPUT_BOTTOM_MARGIN;
    const float chipsHeight = contextChipsHeight(ImGui::GetContentRegionAvail().x);
    const float footerHeight =
        ImGui::GetStyle().ItemSpacing.y + chipsHeight + inputH + bottomMargin;
    const float availHeight =
        std::max(ImGui::GetContentRegionAvail().y - footerHeight, ImGui::GetTextLineHeight());

    const auto& colors = Application::getInstance().getCurrentColors();
    if (ImGui::BeginChild("##ai_side_messages", ImVec2(-1, availHeight), false)) {
        if (agentMissing_ && isAcpBackend()) {
            renderInstallCard();
        }
        if (items_.empty() && !agentMissing_) {
            // the tab warms the agent up on open, so say so instead of showing a hint
            // that invites input the session cannot accept yet
            const std::string warming = agentStartingLabel();
            const std::string text =
                warming.empty() ? "Ask about your database, or type @ to reference a table."
                                : warming;
            const float availW = ImGui::GetContentRegionAvail().x;
            constexpr float spinnerRadius = 6.0f;
            const float spinnerW =
                warming.empty() ? 0.0f : spinnerRadius * 2.0f + Theme::Spacing::S;
            const ImVec2 textSize =
                ImGui::CalcTextSize(text.c_str(), nullptr, false, availW * 0.8f);
            ImGui::SetCursorPos(ImVec2((availW - textSize.x - spinnerW) * 0.5f,
                                       (ImGui::GetContentRegionAvail().y - textSize.y) * 0.5f));
            if (!warming.empty()) {
                UIUtils::Spinner("##ai_warmup", spinnerRadius, 2, ImGui::GetColorU32(colors.peach));
                ImGui::SameLine(0, Theme::Spacing::S);
                ImGui::AlignTextToFramePadding();
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availW * 0.8f);
            ImGui::TextColored(colors.subtext0, "%s", text.c_str());
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

    // pinned context sits above the input box rather than inside its border
    renderContextChips(ImGui::GetContentRegionAvail().x);
    renderInputArea();
}

void AISidebarPanel::renderHeader() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto& cat = agentDefs_;
    const int customIndex = static_cast<int>(cat.size());
    const int apiIndex = customIndex + 1;
    const bool isApi = backendIndex_ == apiIndex;

    const char* currentLabel = backendIndex_ == customIndex ? "Custom agent"
                               : isApi ? "API key"
                                       : cat[static_cast<size_t>(backendIndex_)].name.c_str();

    // size a combo to its longest entry rather than a share of the sidebar
    const auto comboWidth = [&style](const std::vector<std::string>& labels) {
        float widest = 0.0f;
        for (const auto& label : labels) {
            widest = std::max(widest, ImGui::CalcTextSize(label.c_str()).x);
        }
        return widest + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f;
    };

    std::vector<std::string> backendLabels;
    for (const auto& def : cat) {
        backendLabels.push_back(def.name);
    }
    backendLabels.emplace_back("Custom agent");
    backendLabels.emplace_back("API key");

    std::vector<std::string> modelLabels;
    for (const auto& model : API_MODELS) {
        modelLabels.emplace_back(model.label);
    }

    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::XS));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colors.subtext0, ICON_FA_ROBOT);
    ImGui::SameLine(0, Theme::Spacing::S);

    // the sidebar renders inside children with zero WindowPadding, and a combo popup
    // inherits WindowPadding.y -- without this its rows sit flush against the edges
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, Theme::Spacing::S));

    // sessions, new, settings
    const float iconW = ImGui::CalcTextSize(ICON_FA_GEAR).x + style.FramePadding.x * 2.0f;
    const float iconsW = iconW * 3.0f + Theme::Spacing::S * 2.0f;
    const float rowAvail = ImGui::GetContentRegionAvail().x - iconsW - style.ItemSpacing.x;
    float backendW = comboWidth(backendLabels);
    float modelW = isApi ? comboWidth(modelLabels) : 0.0f;
    if (const float wanted = backendW + modelW + (isApi ? style.ItemSpacing.x : 0.0f);
        wanted > rowAvail && wanted > 0.0f) {
        const float scale = rowAvail / wanted;
        backendW *= scale;
        modelW *= scale;
    }

    ImGui::SetNextItemWidth(backendW);
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

    // the model picker shares the row with the backend picker
    if (isApi) {
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::SetNextItemWidth(modelW);
        if (ImGui::BeginCombo("##ai_api_model", API_MODELS[apiModelIndex_].label)) {
            for (int i = 0; i < API_MODEL_COUNT; ++i) {
                if (ImGui::Selectable(API_MODELS[i].label, apiModelIndex_ == i)) {
                    apiModelIndex_ = i;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PopStyleVar();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - iconsW);
    // IconButton keeps the frame transparent and skips the drop shadow, matching the
    // toolbar icons in the table viewer
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    if (UIUtils::IconButton(ICON_FA_CLOCK_ROTATE_LEFT "###ai_sessions")) {
        sessionRows_ = Application::getInstance().getAppState()->getAiSessions(backendId());
        ImGui::OpenPopup("##ai_session_popup");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Sessions");
    }
    renderSessionPopup();
    ImGui::SameLine(0, Theme::Spacing::S);
    if (UIUtils::IconButton(ICON_FA_SQUARE_PLUS "###ai_new_session")) {
        startNewSession();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("New session");
    }
    ImGui::SameLine(0, Theme::Spacing::S);
    if (UIUtils::IconButton(ICON_FA_GEAR "##ai_settings")) {
        AISettingsDialog::instance().show();
    }
    ImGui::PopStyleColor();

    if (backendIndex_ == customIndex) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ai_custom_cmd", "agent command (speaks ACP on stdio)",
                                 customCmdBuf_, sizeof(customCmdBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Application::getInstance().getAppState()->setSetting("ai_custom_agent_cmd",
                                                                 customCmdBuf_);
            stopAgent();
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
            } else if (const AcpInstallOption* install = AcpAgents::resolveInstall(*def)) {
                if (UIUtils::SmallButton(
                        std::format("Install with {}##ai_install", install->label).c_str())) {
                    installer_.start(install->command);
                }
                ImGui::SameLine(0, Theme::Spacing::S);
                ImGui::TextColored(colors.subtext0, "runs: %s", install->command.c_str());
            } else {
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                ImGui::TextColored(colors.subtext0,
                                   "This agent ships as an npm package and no JavaScript runtime "
                                   "was found. Install Node, bun or pnpm - or download an agent "
                                   "below that runs on its own.");
                ImGui::PopTextWrapPos();
            }
        }

        renderRegistryAgents();
        ImGui::Unindent(Theme::Spacing::M);
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
}

std::string AISidebarPanel::agentStartingLabel() const {
    if (!acp_ || !acp_->isRunning() || acp_->isSessionReady()) {
        return "";
    }
    const AcpAgentDef* def = currentAgentDef();
    return std::format("Starting {}...", def ? def->name : std::string("agent"));
}

void AISidebarPanel::renderRegistryAgents() {
#if defined(_WIN32)
    // AcpClient cannot spawn an agent on Windows yet, so downloading one would
    // leave the user with a binary nothing can launch
    return;
#else
    const auto& colors = Application::getInstance().getCurrentColors();

    // fetched once, lazily: nothing hits the network unless an agent is missing
    if (!registryFetchStarted_) {
        registryFetchStarted_ = true;
        registry_.startFetch();
    }
    if (registry_.poll()) {
        // an install may have added an agent; re-resolve the index by id
        const std::string current = backendId();
        agentDefs_ = AcpAgents::availableAgents();
        selectBackend(current);
        if (!registry_.installedId().empty()) {
            items_.push_back({.kind = Item::Kind::Info,
                              .text = "Installed " + registry_.installedId() +
                                      ". Pick it from the agent list above."});
            agentMissing_ = false;
            scrollToBottom_ = true;
        }
    }

    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    ImGui::TextColored(colors.subtext0, "Agents that run without Node");

    if (registry_.isBusy()) {
        UIUtils::Spinner("##registry_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::TextColored(colors.subtext0, "Working...");
        return;
    }
    if (!registry_.error().empty()) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(colors.red, "%s", registry_.error().c_str());
        ImGui::PopTextWrapPos();
    }

    int shown = 0;
    for (const auto& agent : registry_.agents()) {
        if (!agent.hasBinary || AcpRegistry::installedCommand(agent.id)) {
            continue;
        }
        ImGui::PushID(agent.id.c_str());
        if (UIUtils::SmallButton("Download")) {
            registry_.startInstall(agent);
        }
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::TextColored(colors.text, "%s", agent.name.c_str());
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::TextColored(colors.subtext0, "%s", agent.version.c_str());
        ImGui::PopID();
        if (++shown >= 8) {
            break;
        }
    }
    if (shown == 0 && registry_.error().empty()) {
        ImGui::TextColored(colors.subtext0, "None available for this platform.");
    }
#endif
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
        // the list has no left padding; the arc's stroke would clip at the edge
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::Spacing::S);
        UIUtils::Spinner("##ai_side_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        if (const std::string warming = agentStartingLabel(); !warming.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(Application::getInstance().getCurrentColors().subtext0, "%s",
                               warming.c_str());
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
        if (!item.contextNote.empty()) {
            ImGui::TextColored(colors.overlay1, "%s", item.contextNote.c_str());
        }
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
            if (!self->pendingInsertText_.empty()) {
                data->InsertChars(self->pendingReplaceStart_, self->pendingInsertText_.c_str());
            }
            data->CursorPos =
                self->pendingReplaceStart_ + static_cast<int>(self->pendingInsertText_.size());
            self->pendingReplaceStart_ = -1;
            self->pendingReplaceEnd_ = -1;
            self->pendingInsertText_.clear();
        }
        // up/down drive the context picker. imgui forbids CallbackHistory alongside
        // Multiline (both bind the arrows), so the keys are read here and the caret
        // move they caused is undone, leaving the field where it was.
        if (self->mentionOpen_ && !self->mentionMatches_.empty()) {
            int delta = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                delta = -1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                delta = 1;
            }
            if (delta != 0) {
                const int count = static_cast<int>(self->mentionMatches_.size());
                self->mentionSel_ = ((self->mentionSel_ + delta) % count + count) % count;
                data->CursorPos = self->cursorPos_;
                data->SelectionStart = data->SelectionEnd = data->CursorPos;
            }
        }
        self->cursorPos_ = data->CursorPos;
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
    PickerMode mode = PickerMode::None;

    // '/' opens the command list, but only as the first thing in the message so it
    // cannot fire inside ordinary text like "a/b"
    if (!text.empty() && text[0] == '/') {
        const auto space = text.find_first_of(" \t\n");
        if (space == std::string::npos || cursor <= static_cast<int>(space)) {
            at = 0;
            mode = PickerMode::Command;
        }
    }

    if (mode == PickerMode::None) {
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
        if (at >= 0) {
            mode = PickerMode::Context;
        }
    }
    pickerMode_ = mode;

    if (at < 0) {
        mentionDismissedStart_ = -1; // trigger char deleted
    }
    if (at < 0 || at == mentionDismissedStart_) {
        mentionOpen_ = false;
        pickerMode_ = PickerMode::None;
        return;
    }
    if (at != mentionStart_) {
        mentionSel_ = 0;
        mentionDismissedStart_ = -1;
    }
    if (pickerMode_ == PickerMode::Command && at != mentionStart_ && isAcpBackend()) {
        // the agent only publishes its commands after session/new, so there is nothing
        // but our own to show until one is running
        ensureAgentStarted();
    }

    if (pickerMode_ == PickerMode::Context) {
        // rebuild on open, then poll: schema arrives asynchronously, so entries have to
        // be picked up after the fact. throttled so a large schema is not rebuilt every
        // frame
        const double now = ImGui::GetTime();
        if (at != mentionStart_ || contextCandidates_.empty() ||
            now - lastCandidateBuild_ > CONTEXT_REBUILD_INTERVAL) {
            rebuildContextCandidates();
            lastCandidateBuild_ = now;
        }
    }
    mentionStart_ = at;
    mentionFilter_ = text.substr(static_cast<size_t>(at) + 1, static_cast<size_t>(cursor - at - 1));

    std::string needle = mentionFilter_;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    mentionMatches_.clear();
    if (pickerMode_ == PickerMode::Command) {
        rebuildCommandEntries();
        for (int i = 0; i < static_cast<int>(commandEntries_.size()); ++i) {
            if (needle.empty() ||
                commandEntries_[static_cast<size_t>(i)].name.find(needle) != std::string::npos) {
                mentionMatches_.push_back(i);
            }
        }
    } else {
        for (int i = 0; i < static_cast<int>(contextCandidates_.size()); ++i) {
            const auto& item = contextCandidates_[static_cast<size_t>(i)];
            std::string hay = item.name + " " + item.owner;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (needle.empty() || hay.find(needle) != std::string::npos) {
                mentionMatches_.push_back(i);
            }
        }
    }
    // open even with nothing to offer: an empty picker still tells the user why
    mentionOpen_ = true;
    if (mentionSel_ >= static_cast<int>(mentionMatches_.size())) {
        mentionSel_ = 0;
    }
}

void AISidebarPanel::rebuildCommandEntries() {
    commandEntries_.clear();
    for (const auto& cmd : CLIENT_COMMANDS) {
        commandEntries_.push_back({cmd.name, cmd.help, true});
    }
    for (const auto& cmd : agentCommands_) {
        std::string help = cmd.description;
        if (!cmd.hint.empty()) {
            help += "  " + cmd.hint;
        }
        commandEntries_.push_back({cmd.name, help, false});
    }
    // stable, with client entries pushed first, so a name we define wins over an
    // agent command of the same name rather than the order being unspecified
    std::stable_sort(commandEntries_.begin(), commandEntries_.end(),
                     [](const CommandEntry& a, const CommandEntry& b) { return a.name < b.name; });
    commandEntries_.erase(
        std::unique(commandEntries_.begin(), commandEntries_.end(),
                    [](const CommandEntry& a, const CommandEntry& b) { return a.name == b.name; }),
        commandEntries_.end());
}

void AISidebarPanel::acceptPicked(int matchIndex) {
    if (matchIndex < 0 || matchIndex >= static_cast<int>(mentionMatches_.size())) {
        return;
    }
    const int index = mentionMatches_[static_cast<size_t>(matchIndex)];

    if (pickerMode_ == PickerMode::Command) {
        if (index < 0 || index >= static_cast<int>(commandEntries_.size())) {
            return;
        }
        const CommandEntry entry = commandEntries_[static_cast<size_t>(index)];

        // rewrite the "/cmd" token through the input callback: editing inputBuf_
        // directly would be ignored while imgui holds the field active
        pendingReplaceStart_ = 0;
        pendingReplaceEnd_ = static_cast<int>(std::strlen(inputBuf_));
        // an agent command is just prompt text, so leave it in the box for arguments;
        // ours runs here and the box is emptied
        pendingInsertText_ = entry.client ? "" : "/" + entry.name + " ";
        mentionOpen_ = false;
        mentionDismissedStart_ = -1;
        pickerMode_ = PickerMode::None;
        focusInput_ = true;
        if (entry.client) {
            runCommand(entry.name);
        }
        return;
    }
    addContext(contextCandidates_[static_cast<size_t>(index)]);
}

void AISidebarPanel::runCommand(const std::string& name) {
    if (name == "new") {
        startNewSession();
    } else if (name == "clear") {
        apiChat_->cancelAsyncPrompt();
        apiClient_->cancel();
        items_.clear();
        selectedContext_.clear();
    }
}

void AISidebarPanel::addContext(const ContextItem& item) {
    const std::string key = item.key();
    if (std::none_of(selectedContext_.begin(), selectedContext_.end(),
                     [&key](const ContextItem& c) { return c.key() == key; })) {
        selectedContext_.push_back(item);
    }
    // the picker consumed the "@filter" text; drop it from the buffer
    pendingReplaceStart_ = mentionStart_;
    pendingReplaceEnd_ = std::min(cursorPos_, static_cast<int>(std::strlen(inputBuf_)));
    pendingInsertText_.clear();
    mentionOpen_ = false;
    mentionDismissedStart_ = -1;
    focusInput_ = true;
}

void AISidebarPanel::renderMentionPopupAt(float x, float y, float width) {
    if (!mentionOpen_) {
        return;
    }
    const auto& colors = Application::getInstance().getCurrentColors();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        mentionOpen_ = false;
        mentionDismissedStart_ = mentionStart_;
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, INPUT_ROUNDING);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::S, Theme::Spacing::S));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::Spacing::S, Theme::Spacing::S));
    if (ImGui::Begin("##ai_mention_popup", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        // the sidebar takes focus as soon as the user types, which would sort this
        // unfocused window behind it; force it in front without stealing focus away
        // from the text field
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (mentionMatches_.empty()) {
            const char* empty = "No match";
            if (pickerMode_ == PickerMode::Context && contextCandidates_.empty()) {
                empty = "No database connected";
            } else if (pickerMode_ == PickerMode::Command) {
                empty = "No such command";
            }
            ImGui::TextColored(colors.subtext0, "%s", empty);
        }
        const int count = static_cast<int>(mentionMatches_.size());
        const int shown = std::min(count, MENTION_MAX_VISIBLE);
        // scroll the window of rows so the arrow-selected item stays on screen
        const int first = count > shown ? std::clamp(mentionSel_ - shown / 2, 0, count - shown) : 0;

        for (int i = first; i < first + shown; ++i) {
            const int index = mentionMatches_[static_cast<size_t>(i)];
            std::string row;
            std::string trailing;
            if (pickerMode_ == PickerMode::Command) {
                const auto& cmd = commandEntries_[static_cast<size_t>(index)];
                row = std::format("/{}##cmd{}", cmd.name, i);
                trailing = cmd.help;
            } else {
                const auto& item = contextCandidates_[static_cast<size_t>(index)];
                row = std::format("{}  {}##ctx{}", contextKindIcon(item.kind), item.name, i);
                trailing = item.owner;
            }
            if (ImGui::Selectable(row.c_str(), i == mentionSel_)) {
                acceptPicked(i);
            }
            if (!trailing.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(colors.subtext0, "%s", trailing.c_str());
            }
        }
        if (mentionAcceptRequested_) {
            mentionAcceptRequested_ = false;
            acceptPicked(mentionSel_);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(2);
}

float AISidebarPanel::contextChipsHeight(float availWidth) const {
    if (selectedContext_.empty()) {
        return 0.0f;
    }
    const float chipH = chipHeight();
    float x = 0.0f;
    int rows = 1;
    for (const auto& item : selectedContext_) {
        const float w = chipWidth(item.name);
        if (x > 0.0f && x + w > availWidth) {
            ++rows;
            x = 0.0f;
        }
        x += w + Theme::Spacing::XS;
    }
    return static_cast<float>(rows) * chipH + static_cast<float>(rows - 1) * Theme::Spacing::XS +
           Theme::Spacing::XS;
}

void AISidebarPanel::renderContextChips(float availWidth) {
    if (selectedContext_.empty()) {
        return;
    }
    const auto& colors = Application::getInstance().getCurrentColors();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float chipH = chipHeight();
    const float closeW = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    float x = 0.0f;
    float y = 0.0f;
    int removeIndex = -1;

    for (size_t i = 0; i < selectedContext_.size(); ++i) {
        const auto& item = selectedContext_[i];
        const float w = chipWidth(item.name);
        if (x > 0.0f && x + w > availWidth) {
            x = 0.0f;
            y += chipH + Theme::Spacing::XS;
        }
        const ImVec2 p(origin.x + x, origin.y + y);

        drawList->AddRectFilled(p, ImVec2(p.x + w, p.y + chipH),
                                ImGui::GetColorU32(colors.surface2), chipH * 0.5f);
        drawList->AddText(ImVec2(p.x + CHIP_PAD_X, p.y + CHIP_PAD_Y),
                          ImGui::GetColorU32(colors.text), item.name.c_str());

        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - closeW - CHIP_PAD_X, p.y));
        ImGui::InvisibleButton("##ctx_close", ImVec2(closeW + CHIP_PAD_X, chipH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            removeIndex = static_cast<int>(i);
        }
        ImGui::PopID();

        drawList->AddText(ImVec2(p.x + w - closeW - CHIP_PAD_X, p.y + CHIP_PAD_Y),
                          ImGui::GetColorU32(hovered ? colors.red : colors.subtext0),
                          ICON_FA_XMARK);
        x += w + Theme::Spacing::XS;
    }

    if (removeIndex >= 0) {
        selectedContext_.erase(selectedContext_.begin() + removeIndex);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + y + chipH + Theme::Spacing::XS));
}

float AISidebarPanel::computeInputHeight() const {
    const float lineH = ImGui::GetTextLineHeight();
    const float wrapW =
        ImGui::GetContentRegionAvail().x - INPUT_CONTAINER_PAD_X * 2.0f - INPUT_FRAME_PAD_X * 2.0f;

    float textH = lineH;
    if (inputBuf_[0] != '\0' && wrapW > 0.0f) {
        textH = std::max(textH, ImGui::CalcTextSize(inputBuf_, nullptr, false, wrapW).y);
        // CalcTextSize ignores a trailing newline; keep the caret's line visible
        if (inputBuf_[std::strlen(inputBuf_) - 1] == '\n') {
            textH += lineH;
        }
    }
    // a hair of slack so rounding can never make the content outgrow the frame by a
    // pixel, which would pop a scrollbar, narrow the wrap width and add another line
    textH = std::min(textH, lineH * INPUT_MAX_LINES) + INPUT_TEXT_SLACK;

    return INPUT_CONTAINER_PAD_Y * 2.0f + textH + INPUT_FRAME_PAD_Y * 2.0f + INPUT_ROW_GAP +
           inputControlsHeight();
}

void AISidebarPanel::renderInputArea() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const float containerHeight = computeInputHeight();
    const float controlsHeight = inputControlsHeight();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, INPUT_ROUNDING);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(INPUT_CONTAINER_PAD_X, INPUT_CONTAINER_PAD_Y));

    ImVec2 inputRectMin{};
    float inputWidth = 0.0f;

    if (ImGui::BeginChild("##ai_side_input", ImVec2(-1, containerHeight),
                          ImGuiChildFlags_Borders)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(INPUT_FRAME_PAD_X, INPUT_FRAME_PAD_Y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, INPUT_ROW_GAP));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));

        if (focusInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInput_ = false;
        }

        // Enter sends, Shift+Enter inserts a newline (CtrlEnterForNewLine flips Enter's
        // role; imgui always treats Shift+Enter as a newline). CallbackHistory is not
        // allowed alongside Multiline -- both bind up/down -- so mentions are picked with
        // Tab or the mouse instead of the arrow keys.
        const float inputH =
            containerHeight - INPUT_CONTAINER_PAD_Y * 2.0f - INPUT_ROW_GAP - controlsHeight;
        const bool submitted = ImGui::InputTextMultiline(
            "##ai_side_text", inputBuf_, sizeof(inputBuf_), ImVec2(-1, inputH),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
                ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackAlways |
                ImGuiInputTextFlags_CallbackCompletion,
            sidebarInputCallbackThunk, this);
        inputRectMin = ImGui::GetItemRectMin();
        inputWidth = ImGui::GetItemRectSize().x;

        // InputTextMultiline has no hint variant, so draw the placeholder ourselves
        if (inputBuf_[0] == '\0') {
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(inputRectMin.x + INPUT_FRAME_PAD_X, inputRectMin.y + INPUT_FRAME_PAD_Y),
                ImGui::GetColorU32(colors.overlay1), INPUT_HINT);
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2); // FrameBorderSize, FramePadding

        updateMentionState();

        if (submitted) {
            if (mentionOpen_ && !mentionMatches_.empty()) {
                acceptPicked(mentionSel_);
                focusInput_ = true;
            } else {
                sendMessage();
            }
        }

        // controls row, placed by hand so it occupies exactly controlsHeight and the
        // icon lines up with the text's inset above it
        const float rowY = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + INPUT_FRAME_PAD_X,
                                   rowY + (controlsHeight - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(colors.subtext0, ICON_FA_WAND_MAGIC_SPARKLES);

        const float sendBtnWidth = controlsHeight;
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - sendBtnWidth, rowY));

        if (isBusy()) {
            if (UIUtils::IconButton(ICON_FA_STOP, UIUtils::ButtonVariant::Danger,
                                    ImVec2(sendBtnWidth, controlsHeight))) {
                if (acp_) {
                    acp_->cancelTurn();
                    pendingPromptBlocks_ = json::array();
                }
                apiChat_->cancelAsyncPrompt();
                apiClient_->cancel();
            }
        } else {
            if (UIUtils::IconButton(ICON_FA_ARROW_UP, UIUtils::ButtonVariant::Primary,
                                    ImVec2(sendBtnWidth, controlsHeight))) {
                sendMessage();
            }
        }
        ImGui::PopStyleVar(); // ItemSpacing, held through the row gap
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(3); // ChildRounding, ChildBorderSize, WindowPadding
    ImGui::PopStyleColor(2);

    renderMentionPopupAt(inputRectMin.x, inputRectMin.y - Theme::Spacing::S, inputWidth);
}
