#pragma once

#include "database/async_helper.hpp"

#include <atomic>
#include <memory>
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
    // what a stream writes; shared with the worker so the client can go (and the
    // worker be detached) while a request still sits in a connect or read timeout
    struct Sink {
        std::mutex mutex;
        std::string deltas;
        std::string error;
        void append(const std::string& text);
        void fail(const std::string& err);
    };

    AsyncOperation<bool> streamOperation_;
    std::atomic<bool> done_{false};
    std::shared_ptr<Sink> sink_ = std::make_shared<Sink>();

    static void streamAnthropic(Sink& sink, const std::string& apiKey, const std::string& model,
                                const std::string& systemPrompt, std::stop_token stopToken,
                                const std::vector<AIChatMessage>& messages);
    static void streamGemini(Sink& sink, const std::string& apiKey, const std::string& model,
                             const std::string& systemPrompt, std::stop_token stopToken,
                             const std::vector<AIChatMessage>& messages);
    static void streamOpenAI(Sink& sink, const std::string& apiKey, const std::string& model,
                             const std::string& systemPrompt, std::stop_token stopToken,
                             const std::vector<AIChatMessage>& messages);
    void updateCompletionState();
};
