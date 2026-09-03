#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Agent Client Protocol types (https://agentclientprotocol.com). Only the
// fields a client commonly needs are lifted into structs; the full JSON is kept
// in `raw` so nothing from the wire is lost.
namespace acp {

    using json = nlohmann::json;

    constexpr int kProtocolVersion = 1;

    struct RpcError {
        int code = 0;
        std::string message;
        json data;

        [[nodiscard]] std::string describe() const;
    };

    // outcome of a request to the agent
    struct Response {
        json result;
        std::optional<RpcError> error;

        [[nodiscard]] bool ok() const {
            return !error.has_value();
        }
    };

    // what the client advertises in `initialize`
    struct ClientCapabilities {
        bool readTextFile = false;
        bool writeTextFile = false;
        bool terminal = false;

        [[nodiscard]] json toJson() const;
    };

    struct AuthMethod {
        std::string id;
        std::string name;
        std::string description;
    };

    struct AgentCapabilities {
        bool loadSession = false;
        bool promptImage = false;
        bool promptAudio = false;
        bool promptEmbeddedContext = false;
        bool mcpHttp = false;
        bool mcpSse = false;
    };

    struct InitializeResult {
        int protocolVersion = 0;
        AgentCapabilities agentCapabilities;
        std::vector<AuthMethod> authMethods;
        json raw;

        static InitializeResult fromJson(const json& j);
    };

    // MCP server handed to the agent in session/new and session/load
    struct McpServerHttp {
        std::string name;
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;

        [[nodiscard]] json toJson() const;
    };

    struct McpServerStdio {
        std::string name;
        std::string command;
        std::vector<std::string> args;
        std::vector<std::pair<std::string, std::string>> env;

        [[nodiscard]] json toJson() const;
    };

    struct ToolCall {
        std::string id;
        std::string title;
        std::string kind;   // read/edit/execute/search/fetch/...
        std::string status; // pending/in_progress/completed/failed
        json content;       // ToolCallContent array as sent
        json locations;

        // text from every content entry, best effort
        [[nodiscard]] std::string contentText() const;
    };

    struct PlanEntry {
        std::string content;
        std::string priority;
        std::string status;
    };

    struct PermissionOption {
        std::string optionId;
        std::string name;
        std::string kind; // allow_once/allow_always/reject_once/reject_always
    };

    // session/request_permission from the agent. answer through
    // Connection::respondPermission(rpcId, optionId)
    struct PermissionRequest {
        json rpcId;
        std::string sessionId;
        ToolCall toolCall;
        std::vector<PermissionOption> options;
    };

    struct AvailableCommand {
        std::string name;
        std::string description;
        std::string inputHint; // set when the command takes arguments
    };

    // one session/update notification
    struct SessionUpdate {
        enum class Kind {
            UserMessageChunk,
            AgentMessageChunk,
            AgentThoughtChunk,
            ToolCall,
            ToolCallUpdate,
            Plan,
            AvailableCommandsUpdate,
            CurrentModeUpdate,
            Unknown,
        };
        Kind kind = Kind::Unknown;
        std::string text; // chunk text, best effort
        json content;     // the chunk's content block
        ToolCall toolCall;
        std::vector<PlanEntry> plan;
        std::vector<AvailableCommand> commands;
        std::string modeId;
        json raw;

        static SessionUpdate fromJson(const json& j);
    };

    struct SessionNotification {
        std::string sessionId;
        SessionUpdate update;
    };

    // content blocks for prompts
    json textBlock(const std::string& text);
    json resourceBlock(const std::string& uri, const std::string& mimeType,
                       const std::string& text);
    json resourceLinkBlock(const std::string& uri, const std::string& name);

    // text carried by a content block, best effort
    std::string contentBlockText(const json& block);

} // namespace acp
