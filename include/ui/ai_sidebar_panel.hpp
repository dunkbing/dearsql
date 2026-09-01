#pragma once

#include "acp/acp_agents.hpp"
#include "acp/acp_client.hpp"
#include "ai/ai_chat.hpp"
#include "ai/ai_client.hpp"
#include "utils/db_mcp_server.hpp"
#include <memory>
#include <string>
#include <vector>

class DatabaseInterface;
class IDatabaseNode;

// App-level AI chat panel hosted in the sidebar's AI tab. Talks to coding
// agents (Claude Code, Gemini CLI, Codex, custom) via ACP, or falls back to
// the direct API-key client. Supports @table mentions and hands agents a
// read-only MCP query tool over the selected database.
class AISidebarPanel {
public:
    AISidebarPanel();
    ~AISidebarPanel();

    void render();

private:
    // one entry in the transcript
    struct Item {
        enum class Kind { User, Assistant, Thought, Tool, Plan, Permission, Info, Error };
        Kind kind{};
        std::string text;
        // Tool
        std::string toolId;
        std::string toolTitle;
        std::string toolStatus;
        std::string toolOutput;
        bool expanded = false;
        // Plan
        std::vector<AcpPlanEntry> plan;
        // Permission
        AcpPermissionRequest permission;
        bool permissionAnswered = false;
        std::string permissionChoice;
    };

    struct NodeRef {
        std::string label;
        IDatabaseNode* node = nullptr;
    };

    struct MentionEntry {
        std::string name;  // inserted text
        std::string label; // shown in popup (name + node)
        IDatabaseNode* node = nullptr;
    };

    // rendering
    void renderHeader();
    void renderMessages();
    void renderItem(Item& item, size_t index);
    void renderTextWithCodeBlocks(const std::string& content, size_t index);
    void renderInstallCard();
    void renderInputArea();

    // backends
    bool isAcpBackend() const;
    const AcpAgentDef* currentAgentDef() const; // null for custom/api
    std::vector<std::string> currentInvocation(std::string& missingReason);
    void ensureSettingsLoaded();
    void switchBackend(int newIndex);
    void stopAgent();

    // sending
    void sendMessage();
    void sendAcp(const std::string& text);
    void sendApi(const std::string& text);
    void pollAcp();
    void pollApi();

    // context
    std::vector<NodeRef> collectNodes() const;
    IDatabaseNode* contextNode();
    void syncContext(); // track selection changes, update mcp node
    std::string schemaOverview(IDatabaseNode* node) const;
    void rebuildMentionIndex();
    nlohmann::json buildPromptBlocks(const std::string& text);

    // mention popup
    void updateMentionState();
    void acceptMention(const MentionEntry& entry);
    friend int aiSidebarInputCallback(void* dataPtr);
    void renderMentionPopupAt(float x, float y, float width);

    // transcript
    std::vector<Item> items_;
    Item* findToolItem(const std::string& toolId);

    // backend state
    int backendIndex_ = 0; // index into catalog; catalog.size()=custom; +1=api key
    bool settingsLoaded_ = false;
    char customCmdBuf_[512] = {};
    int apiModelIndex_ = 0;
    bool mcpEnabled_ = true;

    std::unique_ptr<AcpClient> acp_;
    AcpAgentInstaller installer_;
    bool agentMissing_ = false;
    std::string agentMissingReason_;
    std::string pendingPromptText_; // queued until session ready

    std::unique_ptr<AIClient> apiClient_;
    std::unique_ptr<AIChatState> apiChat_; // legacy prompt builder / history

    DbMcpServer mcp_;

    // context tracking
    DatabaseInterface* lastDb_ = nullptr;
    int contextNodeIndex_ = 0;
    bool sentSchemaContext_ = false;
    std::vector<MentionEntry> mentionIndex_;

    // input
    char inputBuf_[4096] = {};
    bool focusInput_ = true;
    bool scrollToBottom_ = false;
    int cursorPos_ = 0;
    // mention popup state
    bool mentionOpen_ = false;
    int mentionSel_ = 0;
    int mentionStart_ = -1; // index of '@' in inputBuf_
    std::string mentionFilter_;
    std::vector<int> mentionMatches_;
    // deferred edit applied inside the input callback
    int pendingReplaceStart_ = -1;
    int pendingReplaceEnd_ = -1;
    std::string pendingInsertText_;
    int mentionNavDelta_ = 0;
    bool mentionAcceptRequested_ = false;
    int mentionDismissedStart_ = -1;
};
