#pragma once

#include "ai/ai_chat.hpp"
#include "ai/ai_client.hpp"
#include <functional>
#include <memory>
#include <string>

class AIChatPanel {
public:
    using InsertSQLCallback = std::function<void(const std::string&)>;

    explicit AIChatPanel(AIChatState* chatState);

    void render();
    void setInsertCallback(InsertSQLCallback cb);

private:
    AIChatState* chatState_;
    std::unique_ptr<AIClient> client_;
    InsertSQLCallback insertCallback_;

    char inputBuf_[2048] = {};
    bool scrollToBottom_ = false;
    bool focusInput_ = true;
    int modelIndex_ = 0;
    bool modelSettingsLoaded_ = false;

    void loadModelSettings();
    std::string getSelectedModel() const;
    AIProvider getSelectedProvider() const;

    void sendMessage();
    void renderMessages();
    void renderMessage(const AIChatMessage& msg, size_t index);
    void renderCodeBlock(const std::string& code, const std::string& lang, size_t msgIdx,
                         size_t blockIdx);
    void renderInputArea();
    void pollStreaming();

    // Parse markdown-style code blocks from content
    struct CodeBlock {
        std::string lang;
        std::string code;
        size_t start; // position in original string
        size_t end;
    };
    static std::vector<CodeBlock> parseCodeBlocks(const std::string& content);
};
