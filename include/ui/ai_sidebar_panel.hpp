#pragma once

#include "ai/acp_agents.hpp"
#include "ai/acp_client.hpp"
#include "ai/acp_registry.hpp"
#include "ai/ai_chat.hpp"
#include "ai/ai_client.hpp"
#include "app_state.hpp"
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

    void tick(); // per frame, even when the tab is hidden
    void render();

    // height of the sidebar's History button; the input box matches it so the two
    // top edges line up. zero leaves the input on its default bottom margin.
    void setInputBottomAnchor(float height) {
        inputBottomAnchor_ = height;
    }

private:
    // one entry in the transcript
    struct Item {
        enum class Kind { User, Assistant, Thought, Tool, Plan, Permission, Info, Error };
        Kind kind{};
        std::string text;
        std::string contextNote; // "context: a, b" shown under a user message
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

    // one @-picker candidate, and once chosen, one context pill above the input
    struct ContextItem {
        enum class Kind { Database, Table, View, Sequence };
        Kind kind = Kind::Table;
        std::string name;  // object name, or the node's label for Database
        std::string owner; // owning database/schema, empty for Database
        IDatabaseNode* node = nullptr;

        [[nodiscard]] std::string key() const {
            return std::to_string(static_cast<int>(kind)) + ":" + owner + "." + name;
        }
    };

    // rendering
    void renderHeader();
    void renderMessages();
    [[nodiscard]] std::string agentStartingLabel() const; // empty unless warming up
    void renderItem(Item& item, size_t index);
    void renderTextWithCodeBlocks(const std::string& content, size_t index);
    void renderInstallCard();
    void renderRegistryAgents();
    void renderInputArea();
    [[nodiscard]] float computeInputHeight() const;
    static const char* contextKindIcon(ContextItem::Kind kind);
    void renderContextChips(float availWidth);
    [[nodiscard]] float contextChipsHeight(float availWidth) const;

    // backends
    bool isAcpBackend() const;
    const AcpAgentDef* currentAgentDef() const; // null for custom/api
    std::vector<std::string> currentInvocation(std::string& missingReason);
    void ensureSettingsLoaded();
    void switchBackend(int newIndex);
    // agent id, "custom" or "api"; stable across agentDefs_ rebuilds unlike the index
    [[nodiscard]] std::string backendId() const;
    void selectBackend(const std::string& id);
    void stopAgent();
    [[nodiscard]] bool isBusy() const; // a prompt is queued or being answered

    // sessions: the api transcript is ours, acp agents replay theirs via session/load
    void saveCurrentSession();
    void startNewSession();
    void openSession(const AiSession& session);
    void renderSessionPopup();

    // sending
    void sendMessage();
    void sendAcp(const std::string& text);
    void queueOrSendAcp(nlohmann::json blocks);
    bool ensureAgentStarted();
    void sendApi(const std::string& text);
    void pollAcp();
    void pollApi();

    // context
    std::vector<NodeRef> collectNodes() const;
    IDatabaseNode* contextNode();
    void syncContext(); // track selection changes, update mcp node
    void serviceAgentConnectRequests();
    std::string schemaOverview(IDatabaseNode* node) const;
    void rebuildContextCandidates();
    [[nodiscard]] std::string contextItemText(const ContextItem& item) const;
    nlohmann::json buildPromptBlocks(const std::string& text);

    // input picker: @ pins context, / runs a command
    enum class PickerMode { None, Context, Command };
    void updateMentionState();
    void addContext(const ContextItem& item);
    void acceptPicked(int matchIndex);
    void runCommand(const std::string& name);
    void rebuildCommandEntries();
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
    bool settingsDialogWasOpen_ = false;
    bool agentWarmupDone_ = false;

    // built-in catalog plus registry agents already downloaded; the backend index
    // addresses this list, so it is cached rather than rebuilt per frame
    std::vector<AcpAgentDef> agentDefs_;
    AcpRegistryClient registry_;
    bool registryFetchStarted_ = false;

    std::unique_ptr<AcpClient> acp_;
    AcpAgentInstaller installer_;
    int currentSessionId_ = 0;           // ai_sessions row, 0 until the first turn is saved
    std::string resumeSessionId_;        // acp session to load on the next agent start
    bool wasBusy_ = false;               // saves the session on the busy → idle edge
    std::vector<AiSession> sessionRows_; // fetched when the picker opens
    bool agentMissing_ = false;
    std::string agentMissingReason_;
    nlohmann::json pendingPromptBlocks_ = nlohmann::json::array(); // queued until session ready

    std::unique_ptr<AIClient> apiClient_;
    std::unique_ptr<AIChatState> apiChat_; // legacy prompt builder / history

    DbMcpServer mcp_;

    // context tracking
    // connection the agent asked us to open via the mcp connect_database tool
    std::shared_ptr<DatabaseInterface> pendingConnectDb_;

    DatabaseInterface* lastDb_ = nullptr;
    bool sentSchemaContext_ = false;
    // picker entries: dearsql's own commands plus whatever the agent published via
    // available_commands_update. namespaced like toad's /toad: so ours cannot collide
    // with an agent command such as /clear or /compact.
    struct CommandEntry {
        std::string name;
        std::string help;
        bool client = false; // run locally vs sent to the agent as prompt text
    };
    std::vector<CommandEntry> commandEntries_;
    std::vector<AcpCommand> agentCommands_;

    std::vector<ContextItem> contextCandidates_;
    double lastCandidateBuild_ = 0.0;
    std::vector<ContextItem> selectedContext_;

    // input
    char inputBuf_[4096] = {};
    bool focusInput_ = true;
    bool scrollToBottom_ = false;
    float inputBottomAnchor_ = 0.0f;
    int cursorPos_ = 0;
    // mention popup state
    bool mentionOpen_ = false;
    PickerMode pickerMode_ = PickerMode::None;
    int mentionSel_ = 0;
    int mentionStart_ = -1; // index of '@' in inputBuf_
    std::string mentionFilter_;
    std::vector<int> mentionMatches_;
    // deferred edit applied inside the input callback
    int pendingReplaceStart_ = -1;
    int pendingReplaceEnd_ = -1;
    std::string pendingInsertText_;
    bool mentionAcceptRequested_ = false;
    int mentionDismissedStart_ = -1;
};
