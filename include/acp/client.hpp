#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// Agent Client Protocol (https://agentclientprotocol.com) client.
// Spawns an agent subprocess and speaks newline-delimited JSON-RPC 2.0 over its
// stdio. UI polls drainEvents() each frame; all writes are thread-safe.

struct AcpToolCall {
    std::string id;
    std::string title;
    std::string kind;   // read/edit/execute/search/fetch/...
    std::string status; // pending/in_progress/completed/failed
    std::string output; // extracted text content, best effort
};

struct AcpPlanEntry {
    std::string content;
    std::string priority;
    std::string status;
};

struct AcpPermissionOption {
    std::string optionId;
    std::string name;
    std::string kind; // allow_once/allow_always/reject_once/reject_always
};

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

class AcpClient {
public:
    ~AcpClient();

    // spawn agent + initialize + session/new. mcpUrl (optional) is offered as an
    // HTTP MCP server when the agent advertises support for it.
    // Returns false with error message if the process could not be spawned.
    std::pair<bool, std::string> start(const std::vector<std::string>& argv, const std::string& cwd,
                                       const std::string& mcpUrl, const std::string& mcpName,
                                       const std::string& mcpToken);
    void stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isSessionReady() const;
    [[nodiscard]] bool isTurnActive() const;

    // send a user prompt (array of ACP content blocks). Requires isSessionReady().
    void prompt(const nlohmann::json& contentBlocks);
    void cancelTurn();
    void respondPermission(const nlohmann::json& rpcId, const std::string& optionId);

    std::vector<AcpEvent> drainEvents();

private:
    void readerLoop();
    void stderrLoop();
    void handleMessage(const nlohmann::json& msg);
    void handleResponse(const nlohmann::json& msg);
    void handleSessionUpdate(const nlohmann::json& update);
    void sendMessage(const nlohmann::json& msg);
    long long sendRequest(const std::string& method, const nlohmann::json& params);
    void pushEvent(AcpEvent ev);

    long long pid_ = -1;
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
    int stderrFd_ = -1;
    std::thread reader_;
    std::thread errReader_;

    std::mutex writeMutex_;
    std::mutex stateMutex_; // guards pending_, events_, sessionId_
    std::vector<AcpEvent> events_;
    std::map<long long, std::string> pending_; // request id -> method
    long long nextId_ = 1;

    std::string sessionId_;
    std::string cwd_;
    std::string mcpUrl_;
    std::string mcpName_;
    std::string mcpToken_;
    std::atomic<bool> sessionReady_{false};
    std::atomic<bool> turnActive_{false};
    std::atomic<bool> exited_{false};
    std::atomic<bool> stopping_{false};
};
