#include "ai/ai_client.hpp"
#include "utils/logger.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <format>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

AIClient::~AIClient() {
    cancel();
}

void AIClient::sendStreaming(AIProvider provider, const std::string& apiKey,
                             const std::string& model, const std::string& systemPrompt,
                             const std::vector<AIChatMessage>& messages) {
    if (streamOperation_.isRunning()) {
        return;
    }

    {
        std::lock_guard lock(mutex_);
        deltaBuffer_.clear();
        error_.clear();
    }
    done_ = false;

    streamOperation_.startCancellable(
        [this, provider, apiKey, model, systemPrompt, messages](std::stop_token stopToken) {
            if (provider == AIProvider::ANTHROPIC) {
                streamAnthropic(apiKey, model, systemPrompt, stopToken, messages);
            } else {
                streamGemini(apiKey, model, systemPrompt, stopToken, messages);
            }
            return true;
        });
}

void AIClient::cancel() {
    if (streamOperation_.isRunning()) {
        streamOperation_.cancel();
        updateCompletionState();
    }
}

bool AIClient::isStreaming() const {
    return streamOperation_.isRunning();
}

std::string AIClient::drainDeltas() {
    updateCompletionState();

    std::lock_guard lock(mutex_);
    std::string result = std::move(deltaBuffer_);
    deltaBuffer_.clear();
    return result;
}

bool AIClient::isDone() const {
    const_cast<AIClient*>(this)->updateCompletionState();
    return done_;
}

bool AIClient::consumeDone() {
    updateCompletionState();
    return done_.exchange(false);
}

std::string AIClient::getError() const {
    const_cast<AIClient*>(this)->updateCompletionState();

    std::lock_guard lock(mutex_);
    return error_;
}

void AIClient::appendDelta(const std::string& text) {
    std::lock_guard lock(mutex_);
    deltaBuffer_ += text;
}

void AIClient::finishWithError(const std::string& err) {
    std::lock_guard lock(mutex_);
    error_ = err;
    Logger::error(std::format("AIClient: {}", err));
}

void AIClient::updateCompletionState() {
    if (!done_ && streamOperation_.check()) {
        streamOperation_.waitAndGet();
        done_ = true;
    }
}

void AIClient::streamAnthropic(const std::string& apiKey, const std::string& model,
                               const std::string& systemPrompt, std::stop_token stopToken,
                               const std::vector<AIChatMessage>& messages) {
    httplib::Client cli("https://api.anthropic.com");
    cli.set_read_timeout(60);
    cli.set_connection_timeout(10);

    json body;
    body["model"] = model;
    body["max_tokens"] = 4096;
    body["stream"] = true;

    if (!systemPrompt.empty()) {
        body["system"] = systemPrompt;
    }

    json msgs = json::array();
    for (const auto& m : messages) {
        msgs.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["messages"] = msgs;

    httplib::Headers headers = {
        {"x-api-key", apiKey},
        {"anthropic-version", "2023-06-01"},
        {"content-type", "application/json"},
    };

    std::string lineBuffer;

    auto res = cli.Post("/v1/messages", headers, body.dump(), "application/json",
                        [&](const char* data, size_t len) -> bool {
                            if (stopToken.stop_requested()) {
                                return false;
                            }

                            lineBuffer.append(data, len);

                            size_t pos = 0;
                            while (true) {
                                auto nl = lineBuffer.find('\n', pos);
                                if (nl == std::string::npos) {
                                    break;
                                }

                                std::string line = lineBuffer.substr(pos, nl - pos);
                                pos = nl + 1;

                                if (!line.empty() && line.back() == '\r') {
                                    line.pop_back();
                                }

                                if (line.starts_with("data: ")) {
                                    std::string jsonStr = line.substr(6);
                                    if (jsonStr == "[DONE]") {
                                        continue;
                                    }
                                    try {
                                        auto event = json::parse(jsonStr);
                                        if (event.value("type", "") == "content_block_delta") {
                                            auto delta = event.value("delta", json::object());
                                            if (delta.value("type", "") == "text_delta") {
                                                appendDelta(delta.value("text", ""));
                                            }
                                        } else if (event.value("type", "") == "error") {
                                            auto errObj = event.value("error", json::object());
                                            finishWithError(
                                                errObj.value("message", "Unknown Anthropic error"));
                                            return false;
                                        }
                                    } catch (...) {
                                        // Skip malformed JSON
                                    }
                                }
                            }

                            lineBuffer = lineBuffer.substr(pos);
                            return true;
                        });

    if (!res) {
        if (!stopToken.stop_requested()) {
            finishWithError("Connection to Anthropic API failed");
        }
    } else if (res->status != 200 && !stopToken.stop_requested()) {
        try {
            auto errBody = json::parse(res->body);
            auto errObj = errBody.value("error", json::object());
            finishWithError(std::format("Anthropic API error ({}): {}", res->status,
                                        errObj.value("message", res->body)));
        } catch (...) {
            finishWithError(std::format("Anthropic API error ({}): {}", res->status, res->body));
        }
    }
}

void AIClient::streamGemini(const std::string& apiKey, const std::string& model,
                            const std::string& systemPrompt, std::stop_token stopToken,
                            const std::vector<AIChatMessage>& messages) {
    httplib::Client cli("https://generativelanguage.googleapis.com");
    cli.set_read_timeout(60);
    cli.set_connection_timeout(10);

    json body;

    if (!systemPrompt.empty()) {
        body["system_instruction"] = {{"parts", {{{"text", systemPrompt}}}}};
    }

    json contents = json::array();
    for (const auto& m : messages) {
        std::string role = (m.role == "assistant") ? "model" : "user";
        contents.push_back({{"role", role}, {"parts", {{{"text", m.content}}}}});
    }
    body["contents"] = contents;

    std::string path =
        std::format("/v1beta/models/{}:streamGenerateContent?alt=sse&key={}", model, apiKey);

    httplib::Headers headers = {{"content-type", "application/json"}};

    std::string lineBuffer;

    auto res = cli.Post(
        path, headers, body.dump(), "application/json", [&](const char* data, size_t len) -> bool {
            if (stopToken.stop_requested()) {
                return false;
            }

            lineBuffer.append(data, len);

            size_t pos = 0;
            while (true) {
                auto nl = lineBuffer.find('\n', pos);
                if (nl == std::string::npos) {
                    break;
                }

                std::string line = lineBuffer.substr(pos, nl - pos);
                pos = nl + 1;

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.starts_with("data: ")) {
                    std::string jsonStr = line.substr(6);
                    try {
                        auto event = json::parse(jsonStr);
                        auto candidates = event.value("candidates", json::array());
                        if (!candidates.empty()) {
                            auto content = candidates[0].value("content", json::object());
                            auto parts = content.value("parts", json::array());
                            if (!parts.empty()) {
                                appendDelta(parts[0].value("text", ""));
                            }
                        }

                        if (event.contains("error")) {
                            auto errObj = event["error"];
                            finishWithError(errObj.value("message", "Unknown Gemini error"));
                            return false;
                        }
                    } catch (...) {
                        // Skip malformed JSON
                    }
                }
            }

            lineBuffer = lineBuffer.substr(pos);
            return true;
        });

    if (!res) {
        if (!stopToken.stop_requested()) {
            finishWithError("Connection to Gemini API failed");
        }
    } else if (res->status != 200 && !stopToken.stop_requested()) {
        try {
            auto errBody = json::parse(res->body);
            auto errObj = errBody.value("error", json::object());
            finishWithError(std::format("Gemini API error ({}): {}", res->status,
                                        errObj.value("message", res->body)));
        } catch (...) {
            finishWithError(std::format("Gemini API error ({}): {}", res->status, res->body));
        }
    }
}
