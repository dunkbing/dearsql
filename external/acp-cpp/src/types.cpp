#include "acp/types.hpp"

namespace acp {

    std::string RpcError::describe() const {
        std::string text = message.empty() ? "Agent error" : message;
        if (data.is_object() && data.contains("details")) {
            text += ": " + data["details"].dump();
        }
        return text;
    }

    json ClientCapabilities::toJson() const {
        return {{"fs", {{"readTextFile", readTextFile}, {"writeTextFile", writeTextFile}}},
                {"terminal", terminal}};
    }

    InitializeResult InitializeResult::fromJson(const json& j) {
        InitializeResult out;
        out.raw = j;
        out.protocolVersion = j.value("protocolVersion", 0);
        const json caps = j.value("agentCapabilities", json::object());
        out.agentCapabilities.loadSession = caps.value("loadSession", false);
        const json prompt = caps.value("promptCapabilities", json::object());
        out.agentCapabilities.promptImage = prompt.value("image", false);
        out.agentCapabilities.promptAudio = prompt.value("audio", false);
        out.agentCapabilities.promptEmbeddedContext = prompt.value("embeddedContext", false);
        const json mcp = caps.value("mcpCapabilities", json::object());
        out.agentCapabilities.mcpHttp = mcp.value("http", false);
        out.agentCapabilities.mcpSse = mcp.value("sse", false);
        for (const auto& m : j.value("authMethods", json::array())) {
            out.authMethods.push_back(
                {m.value("id", ""), m.value("name", ""), m.value("description", "")});
        }
        return out;
    }

    json McpServerHttp::toJson() const {
        json hdrs = json::array();
        for (const auto& [name, value] : headers) {
            hdrs.push_back({{"name", name}, {"value", value}});
        }
        return {{"type", "http"}, {"name", name}, {"url", url}, {"headers", hdrs}};
    }

    json McpServerStdio::toJson() const {
        json envs = json::array();
        for (const auto& [name, value] : env) {
            envs.push_back({{"name", name}, {"value", value}});
        }
        return {{"name", name}, {"command", command}, {"args", args}, {"env", envs}};
    }

    std::string contentBlockText(const json& block) {
        if (!block.is_object()) {
            return "";
        }
        const std::string type = block.value("type", "");
        if (type == "text") {
            return block.value("text", "");
        }
        if (type == "resource") {
            return block.value("resource", json::object()).value("text", "");
        }
        if (type == "resource_link") {
            return block.value("uri", "");
        }
        return "";
    }

    std::string ToolCall::contentText() const {
        std::string out;
        if (!content.is_array()) {
            return out;
        }
        for (const auto& item : content) {
            const std::string type = item.value("type", "");
            if (type == "content") {
                out += contentBlockText(item.value("content", json::object()));
            } else if (type == "diff") {
                out += "diff: " + item.value("path", "");
            } else if (type == "terminal") {
                out += "[terminal output]";
            }
            if (!out.empty() && out.back() != '\n') {
                out += "\n";
            }
        }
        return out;
    }

    namespace {
        ToolCall toolCallFromJson(const json& j) {
            ToolCall tc;
            tc.id = j.value("toolCallId", "");
            tc.title = j.value("title", "");
            tc.kind = j.value("kind", "");
            tc.status = j.value("status", "");
            tc.content = j.value("content", json::array());
            tc.locations = j.value("locations", json::array());
            return tc;
        }
    } // namespace

    SessionUpdate SessionUpdate::fromJson(const json& j) {
        SessionUpdate u;
        u.raw = j;
        const std::string kind = j.value("sessionUpdate", "");
        if (kind == "user_message_chunk" || kind == "agent_message_chunk" ||
            kind == "agent_thought_chunk") {
            u.kind = kind == "user_message_chunk"    ? Kind::UserMessageChunk
                     : kind == "agent_message_chunk" ? Kind::AgentMessageChunk
                                                     : Kind::AgentThoughtChunk;
            u.content = j.value("content", json::object());
            u.text = contentBlockText(u.content);
        } else if (kind == "tool_call" || kind == "tool_call_update") {
            u.kind = kind == "tool_call" ? Kind::ToolCall : Kind::ToolCallUpdate;
            u.toolCall = toolCallFromJson(j);
        } else if (kind == "plan") {
            u.kind = Kind::Plan;
            for (const auto& e : j.value("entries", json::array())) {
                u.plan.push_back(
                    {e.value("content", ""), e.value("priority", ""), e.value("status", "")});
            }
        } else if (kind == "available_commands_update") {
            u.kind = Kind::AvailableCommandsUpdate;
            for (const auto& cmd : j.value("availableCommands", json::array())) {
                AvailableCommand out;
                out.name = cmd.value("name", "");
                out.description = cmd.value("description", "");
                if (cmd.contains("input") && cmd["input"].is_object()) {
                    out.inputHint = cmd["input"].value("hint", "");
                }
                if (!out.name.empty()) {
                    u.commands.push_back(std::move(out));
                }
            }
        } else if (kind == "current_mode_update") {
            u.kind = Kind::CurrentModeUpdate;
            u.modeId = j.value("currentModeId", "");
        }
        return u;
    }

    json textBlock(const std::string& text) {
        return {{"type", "text"}, {"text", text}};
    }

    json resourceBlock(const std::string& uri, const std::string& mimeType,
                       const std::string& text) {
        return {{"type", "resource"},
                {"resource", {{"uri", uri}, {"mimeType", mimeType}, {"text", text}}}};
    }

    json resourceLinkBlock(const std::string& uri, const std::string& name) {
        return {{"type", "resource_link"}, {"uri", uri}, {"name", name}};
    }

} // namespace acp
