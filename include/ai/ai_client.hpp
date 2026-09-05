#pragma once

#include "database/async_helper.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

enum class AIProvider { ANTHROPIC, OPENAI, GEMINI };

// app_settings key holding the api key for a provider
inline const char* apiKeySettingFor(AIProvider provider) {
    switch (provider) {
    case AIProvider::OPENAI:
        return "ai_api_key_openai";
    case AIProvider::GEMINI:
        return "ai_api_key_gemini";
    default:
        return "ai_api_key_anthropic";
    }
}

struct AIChatMessage {
    std::string role; // "user" or "assistant"
    std::string content;
};

class AIClient {
public:
    ~AIClient();

    void sendStreaming(AIProvider provider, const std::string& apiKey, const std::string& model,
                       const std::string& systemPrompt, const std::vector<AIChatMessage>& messages);
    void cancel();
    [[nodiscard]] bool isStreaming() const;

    // Main-thread polling
    std::string drainDeltas();
    [[nodiscard]] bool isDone() const;
    bool consumeDone();
    [[nodiscard]] std::string getError() const;

private:
    AsyncOperation<bool> streamOperation_;
    std::atomic<bool> done_{false};
    mutable std::mutex mutex_;
    std::string deltaBuffer_;
    std::string error_;

    void streamAnthropic(const std::string& apiKey, const std::string& model,
                         const std::string& systemPrompt, std::stop_token stopToken,
                         const std::vector<AIChatMessage>& messages);
    void streamGemini(const std::string& apiKey, const std::string& model,
                      const std::string& systemPrompt, std::stop_token stopToken,
                      const std::vector<AIChatMessage>& messages);
    void streamOpenAI(const std::string& apiKey, const std::string& model,
                      const std::string& systemPrompt, std::stop_token stopToken,
                      const std::vector<AIChatMessage>& messages);
    void appendDelta(const std::string& text);
    void finishWithError(const std::string& err);
    void updateCompletionState();
};
