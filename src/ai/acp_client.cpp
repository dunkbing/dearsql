#include "ai/acp_client.hpp"
#include "ai/acp_agents.hpp"

#include <algorithm>
#include <cstdlib>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

AcpClient::~AcpClient() {
    stop();
}

std::pair<bool, std::string>
AcpClient::start(const std::vector<std::string>& argv, const std::string& cwd,
                 const std::string& mcpUrl, const std::string& mcpName, const std::string& mcpToken,
                 const std::string& resumeSessionId,
                 const std::vector<std::pair<std::string, std::string>>& extraEnv) {
    if (conn_ && conn_->isRunning()) {
        return {true, ""};
    }
    cwd_ = cwd;
    mcpUrl_ = mcpUrl;
    mcpName_ = mcpName;
    mcpToken_ = mcpToken;
    resumeSessionId_ = resumeSessionId;

    acp::SpawnOptions opts;
    opts.cwd = cwd;
    // GUI apps on macOS get a minimal PATH; use the login shell's so agents
    // installed via npm/homebrew are found
    if (const std::string& path = acp::agents::loginShellPath(); !path.empty()) {
        opts.env.emplace_back("PATH", path);
    }
    // claude refuses to start when it thinks it is nested in another session;
    // inherited when DearSQL itself was launched from one
    opts.dropEnv = {"CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT"};
    // api keys saved in AI Settings; a key already in the environment wins
    exportedApiKey_ = false;
    for (const auto& [name, value] : extraEnv) {
        if (!value.empty() && !std::getenv(name.c_str())) {
            opts.env.emplace_back(name, value);
            exportedApiKey_ = true;
        }
    }
    authTried_ = false;

    auto [conn, err] = acp::Connection::spawn(*this, argv, opts);
    if (!conn) {
        return {false, err};
    }
    conn_ = std::move(conn);
    // DearSQL is a database client, not an editor: no fs, no terminal
    conn_->initialize(acp::ClientCapabilities{}, acp::kProtocolVersion,
                      [this](acp::Response r) { onInitialized(r); });
    return {true, ""};
}

void AcpClient::onInitialized(const acp::Response& r) {
    if (!r.ok()) {
        pushEvent({.type = AcpEvent::Type::Error, .text = r.error->describe()});
        return;
    }
    const auto init = acp::InitializeResult::fromJson(r.result);
    loadSession_ = init.agentCapabilities.loadSession;
    authMethods_ = init.authMethods;

    mcpServers_ = json::array();
    if (!mcpUrl_.empty() && init.agentCapabilities.mcpHttp) {
        acp::McpServerHttp server;
        server.name = mcpName_;
        server.url = mcpUrl_;
        if (!mcpToken_.empty()) {
            server.headers.emplace_back("Authorization", "Bearer " + mcpToken_);
        }
        mcpServers_.push_back(server.toJson());
    }
    openSession();
}

void AcpClient::openSession() {
    if (!resumeSessionId_.empty() && loadSession_) {
        conn_->loadSession(resumeSessionId_, cwd_, mcpServers_,
                           [this](acp::Response res) { onSessionOpened("session/load", res); });
        return;
    }
    if (!resumeSessionId_.empty()) {
        pushEvent({.type = AcpEvent::Type::Error,
                   .text = "This agent cannot resume sessions. Starting a new one."});
        resumeSessionId_.clear();
    }
    conn_->newSession(cwd_, mcpServers_,
                      [this](acp::Response res) { onSessionOpened("session/new", res); });
}

// acp: session/new answers -32000 auth_required until authenticate() succeeds. api-key
// methods read the key from the env; oauth ones open the browser from the agent side
void AcpClient::authenticateAndRetry(const acp::RpcError& err) {
    authTried_ = true;
    const acp::AuthMethod* method = &authMethods_.front();
    if (exportedApiKey_) {
        for (const auto& m : authMethods_) {
            if (m.id.find("api") != std::string::npos) {
                method = &m;
                break;
            }
        }
    }
    pushEvent({.type = AcpEvent::Type::Info, .text = "Signing in: " + method->name + "..."});
    spdlog::info("ACP: authenticating with {} after: {}", method->id, err.describe());
    conn_->authenticate(method->id, [this](acp::Response res) {
        if (!res.ok()) {
            pushEvent({.type = AcpEvent::Type::Error,
                       .text = "Sign-in failed: " + res.error->describe()});
            return;
        }
        openSession();
    });
}

void AcpClient::onSessionOpened(const std::string& method, const acp::Response& r) {
    if (!r.ok()) {
        std::string lower = r.error->message;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        const bool authRequired =
            r.error->code == -32000 || lower.find("auth") != std::string::npos;
        if (authRequired && !authTried_ && !authMethods_.empty()) {
            authenticateAndRetry(*r.error);
            return;
        }
        if (method == "session/load") {
            // the agent pruned it (or never had it); fall back to a fresh session
            pushEvent({.type = AcpEvent::Type::Error,
                       .text = "Could not resume the session: " + r.error->describe() +
                               ". Starting a new one."});
            conn_->newSession(cwd_, mcpServers_,
                              [this](acp::Response res) { onSessionOpened("session/new", res); });
            return;
        }
        pushEvent({.type = AcpEvent::Type::Error, .text = r.error->describe()});
        return;
    }
    {
        std::lock_guard lock(stateMutex_);
        sessionId_ = method == "session/load" ? resumeSessionId_ : r.result.value("sessionId", "");
    }
    sessionReady_ = true;
    pushEvent({.type = AcpEvent::Type::SessionReady});
}

void AcpClient::stop() {
    if (conn_) {
        conn_->stop();
        conn_.reset();
    }
    sessionReady_ = false;
    turnActive_ = false;
}

bool AcpClient::isRunning() const {
    return conn_ && conn_->isRunning();
}

bool AcpClient::isSessionReady() const {
    return sessionReady_ && isRunning();
}

bool AcpClient::isTurnActive() const {
    return turnActive_ && isRunning();
}

std::string AcpClient::sessionId() {
    std::lock_guard lock(stateMutex_);
    return sessionId_;
}

void AcpClient::prompt(const json& contentBlocks) {
    if (!isSessionReady() || turnActive_) {
        return;
    }
    turnActive_ = true;
    conn_->prompt(sessionId(), contentBlocks, [this](acp::Response r) {
        turnActive_ = false;
        if (!r.ok()) {
            pushEvent({.type = AcpEvent::Type::Error, .text = r.error->describe()});
            return;
        }
        pushEvent({.type = AcpEvent::Type::TurnEnded, .text = r.result.value("stopReason", "")});
    });
}

void AcpClient::cancelTurn() {
    if (isSessionReady()) {
        conn_->cancel(sessionId());
    }
}

void AcpClient::respondPermission(const json& rpcId, const std::string& optionId) {
    if (conn_) {
        conn_->respondPermission(rpcId, optionId);
    }
}

std::vector<AcpEvent> AcpClient::drainEvents() {
    std::lock_guard lock(stateMutex_);
    std::vector<AcpEvent> out;
    out.swap(events_);
    return out;
}

void AcpClient::pushEvent(AcpEvent ev) {
    std::lock_guard lock(stateMutex_);
    events_.push_back(std::move(ev));
}

// ---------------------------------------------------------------- acp::Client

void AcpClient::sessionUpdate(const acp::SessionNotification& n) {
    using K = acp::SessionUpdate::Kind;
    const auto& u = n.update;
    AcpEvent ev;
    switch (u.kind) {
    case K::UserMessageChunk:
    case K::AgentMessageChunk:
    case K::AgentThoughtChunk:
        if (u.text.empty()) {
            return;
        }
        ev.type = u.kind == K::UserMessageChunk    ? AcpEvent::Type::UserMessageDelta
                  : u.kind == K::AgentMessageChunk ? AcpEvent::Type::MessageDelta
                                                   : AcpEvent::Type::ThoughtDelta;
        ev.text = u.text;
        break;
    case K::ToolCall:
    case K::ToolCallUpdate:
        ev.type = u.kind == K::ToolCall ? AcpEvent::Type::ToolCall : AcpEvent::Type::ToolCallUpdate;
        ev.tool = {u.toolCall.id, u.toolCall.title, u.toolCall.kind, u.toolCall.status,
                   u.toolCall.contentText()};
        break;
    case K::Plan:
        ev.type = AcpEvent::Type::Plan;
        ev.plan = u.plan;
        break;
    case K::AvailableCommandsUpdate:
        ev.type = AcpEvent::Type::Commands;
        for (const auto& c : u.commands) {
            ev.commands.push_back({c.name, c.description, c.inputHint});
        }
        break;
    default:
        return; // current_mode_update and unknown kinds
    }
    pushEvent(std::move(ev));
}

void AcpClient::requestPermission(const acp::PermissionRequest& req) {
    AcpEvent ev;
    ev.type = AcpEvent::Type::Permission;
    ev.permission.rpcId = req.rpcId;
    ev.permission.title = req.toolCall.title.empty() ? "Agent action" : req.toolCall.title;
    ev.permission.options = req.options;
    pushEvent(std::move(ev));
}

void AcpClient::agentStderr(const std::string& line) {
    spdlog::debug("ACP agent stderr: {}", line);
}

void AcpClient::agentExited() {
    sessionReady_ = false;
    turnActive_ = false;
    pushEvent({.type = AcpEvent::Type::Exited});
}
