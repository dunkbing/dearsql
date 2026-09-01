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
    void renderInputArea();
    void pollStreaming();
};
