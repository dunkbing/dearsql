#pragma once

#include <acp/connection.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// DearSQL's view of an ACP agent, built on external/acp-cpp. The library calls
// back on its reader thread; this adapter turns those calls into AcpEvents that
// the UI drains once per frame, and runs the initialize → session/new|load
// handshake so the panel only has to wait for SessionReady.

struct AcpToolCall {
    std::string id;
    std::string title;
    std::string kind;   // read/edit/execute/search/fetch/...
    std::string status; // pending/in_progress/completed/failed
    std::string output; // extracted text content, best effort
};
using AcpPlanEntry = acp::PlanEntry;
using AcpPermissionOption = acp::PermissionOption;

struct AcpPermissionRequest {
    nlohmann::json rpcId; // JSON-RPC id to answer with
    std::string title;
    std::vector<AcpPermissionOption> options;
};

struct AcpCommand {
    std::string name;
    std::string description;
    std::string hint; // shown when the command takes arguments
};

struct AcpEvent {
    enum class Type {
        SessionReady,
        Info,             // text = progress note (signing in, ...)
        UserMessageDelta, // replayed by session/load
        MessageDelta,
        ThoughtDelta,
        ToolCall,
        ToolCallUpdate,
        Plan,
        Commands, // agent published its slash commands
        Permission,
        TurnEnded, // text = stopReason
        Error,     // text = message
        Exited,    // agent process died
    };
    Type type{};
    std::string text;
    AcpToolCall tool;
    std::vector<AcpPlanEntry> plan;
    std::vector<AcpCommand> commands;
    AcpPermissionRequest permission;
};

class AcpClient final : public acp::Client {
public:
    ~AcpClient() override;

    // spawn agent + initialize + session/new. mcpUrl (optional) is offered as an
    // HTTP MCP server when the agent advertises support for it. resumeSessionId
    // asks for session/load instead when the agent supports it (history is replayed
    // as events); otherwise a new session is started.
    // Returns false with error message if the process could not be spawned.
    std::pair<bool, std::string>
    start(const std::vector<std::string>& argv, const std::string& cwd, const std::string& mcpUrl,
          const std::string& mcpName, const std::string& mcpToken,
          const std::string& resumeSessionId = "",
          const std::vector<std::pair<std::string, std::string>>& extraEnv = {});
    void stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isSessionReady() const;
    [[nodiscard]] bool isTurnActive() const;
    [[nodiscard]] bool supportsLoadSession() const {
        return loadSession_;
    }
    std::string sessionId();

    // send a user prompt (array of ACP content blocks). Requires isSessionReady().
    void prompt(const nlohmann::json& contentBlocks);
    void cancelTurn();
    void respondPermission(const nlohmann::json& rpcId, const std::string& optionId);

    std::vector<AcpEvent> drainEvents();

private:
    // acp::Client
    void sessionUpdate(const acp::SessionNotification& n) override;
    void requestPermission(const acp::PermissionRequest& req) override;
    void agentStderr(const std::string& line) override;
    void agentExited() override;

    void onInitialized(const acp::Response& r);
    void openSession();
    void authenticateAndRetry(const acp::RpcError& err);
    void onSessionOpened(const std::string& method, const acp::Response& r);
    void pushEvent(AcpEvent ev);

    std::unique_ptr<acp::Connection> conn_;
    std::mutex stateMutex_; // guards events_, sessionId_
    std::vector<AcpEvent> events_;
    std::string sessionId_;
    std::string resumeSessionId_;
    std::string cwd_;
    std::string mcpUrl_;
    std::string mcpName_;
    std::string mcpToken_;
    nlohmann::json mcpServers_ = nlohmann::json::array();
    std::vector<acp::AuthMethod> authMethods_;
    bool exportedApiKey_ = false; // an api key went into the agent env, prefer that auth method
    bool authTried_ = false;
    std::atomic<bool> loadSession_{false};
    std::atomic<bool> sessionReady_{false};
    std::atomic<bool> turnActive_{false};
};
