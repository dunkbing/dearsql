#include "ai/ai_client.hpp"
#include <spdlog/spdlog.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <format>
#include <nlohmann/json.hpp>
#include <utility>

using json = nlohmann::json;

// a request may sit in its 10s connect / 60s read timeout, and the stop token is
// only seen between chunks: never join it, the worker owns what it writes to
AIClient::~AIClient() {
    cancel();
    streamOperation_.detach();
}

void AIClient::sendStreaming(AIProvider provider, const std::string& apiKey,
                             const std::string& model, const std::string& systemPrompt,
                             const std::vector<AIChatMessage>& messages) {
    if (streamOperation_.isRunning()) {
        return;
    }

    // a fresh sink: a cancelled stream still unwinding writes only to its own
    sink_ = std::make_shared<Sink>();
    done_ = false;

    streamOperation_.startCancellable(
        [sink = sink_, provider, apiKey, model, systemPrompt, messages](std::stop_token stopToken) {
            if (provider == AIProvider::ANTHROPIC) {
                streamAnthropic(*sink, apiKey, model, systemPrompt, stopToken, messages);
            } else if (provider == AIProvider::OPENAI) {
                streamOpenAI(*sink, apiKey, model, systemPrompt, stopToken, messages);
            } else {
                streamGemini(*sink, apiKey, model, systemPrompt, stopToken, messages);
            }
            return true;
        });
}

void AIClient::cancel() {
    if (streamOperation_.isRunning()) {
        streamOperation_.cancel();
    }
    done_ = true;
}

bool AIClient::isStreaming() const {
    return streamOperation_.isRunning();
}

std::string AIClient::drainDeltas() {
    updateCompletionState();

    std::lock_guard lock(sink_->mutex);
    return std::exchange(sink_->deltas, {});
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

    std::lock_guard lock(sink_->mutex);
    return sink_->error;
}

void AIClient::Sink::append(const std::string& text) {
    std::lock_guard lock(mutex);
    deltas += text;
}

void AIClient::Sink::fail(const std::string& err) {
    std::lock_guard lock(mutex);
    error = err;
    spdlog::error("AIClient: {}", err);
}

void AIClient::updateCompletionState() {
    if (!done_ && streamOperation_.check()) {
        streamOperation_.waitAndGet();
        done_ = true;
    }
}

void AIClient::streamAnthropic(Sink& sink, const std::string& apiKey, const std::string& model,
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

    auto res =
        cli.Post("/v1/messages", headers, body.dump(), "application/json",
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
                                         sink.append(delta.value("text", ""));
                                     }
                                 } else if (event.value("type", "") == "error") {
                                     auto errObj = event.value("error", json::object());
                                     sink.fail(errObj.value("message", "Unknown Anthropic error"));
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
            sink.fail("Connection to Anthropic API failed");
        }
    } else if (res->status != 200 && !stopToken.stop_requested()) {
        try {
            auto errBody = json::parse(res->body);
            auto errObj = errBody.value("error", json::object());
            sink.fail(std::format("Anthropic API error ({}): {}", res->status,
                                  errObj.value("message", res->body)));
        } catch (...) {
            sink.fail(std::format("Anthropic API error ({}): {}", res->status, res->body));
        }
    }
}

void AIClient::streamGemini(Sink& sink, const std::string& apiKey, const std::string& model,
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
                                sink.append(parts[0].value("text", ""));
                            }
                        }

                        if (event.contains("error")) {
                            auto errObj = event["error"];
                            std::string errMsg = errObj.value("message", "Unknown Gemini error");
                            spdlog::error("Gemini Stream Error Object: {}", event.dump());
                            sink.fail(errMsg);
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
            sink.fail("Connection to Gemini API failed completely (network issue)");
            spdlog::error("Gemini API Connection failed entirely");
        }
    } else if (res->status != 200 && !stopToken.stop_requested()) {
        spdlog::error("Gemini API HTTP Error {}: {}", res->status, res->body);
        try {
            auto errBody = json::parse(res->body);
            auto errObj = errBody.value("error", json::object());
            std::string errMsg = errObj.value("message", res->body);

            // Check if it's the model not found error and append a helpful message
            if (res->status == 404 && errMsg.find("models/") != std::string::npos) {
                errMsg +=
                    "\nHint: Check if the selected model is supported by your API key/project.";
            }

            sink.fail(std::format("Gemini API error ({}): {}", res->status, errMsg));
        } catch (...) {
            sink.fail(std::format("Gemini API error ({}): {}", res->status, res->body));
        }
    }
}

// responses api: server-sent events, text arrives as response.output_text.delta
void AIClient::streamOpenAI(Sink& sink, const std::string& apiKey, const std::string& model,
                            const std::string& systemPrompt, std::stop_token stopToken,
                            const std::vector<AIChatMessage>& messages) {
    httplib::Client cli("https://api.openai.com");
    cli.set_read_timeout(60);
    cli.set_connection_timeout(10);

    json body;
    body["model"] = model;
    body["stream"] = true;
    if (!systemPrompt.empty()) {
        body["instructions"] = systemPrompt;
    }
    json input = json::array();
    for (const auto& m : messages) {
        input.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["input"] = input;

    httplib::Headers headers = {
        {"Authorization", "Bearer " + apiKey},
        {"content-type", "application/json"},
    };

    std::string lineBuffer;
    auto res = cli.Post("/v1/responses", headers, body.dump(), "application/json",
                        [&](const char* data, size_t len) -> bool {
                            if (stopToken.stop_requested()) {
                                return false;
                            }
                            lineBuffer.append(data, len);
                            size_t pos = 0;
                            while (true) {
                                const auto nl = lineBuffer.find('\n', pos);
                                if (nl == std::string::npos) {
                                    break;
                                }
                                std::string line = lineBuffer.substr(pos, nl - pos);
                                pos = nl + 1;
                                if (!line.empty() && line.back() == '\r') {
                                    line.pop_back();
                                }
                                if (!line.starts_with("data: ")) {
                                    continue;
                                }
                                const std::string jsonStr = line.substr(6);
                                if (jsonStr == "[DONE]") {
                                    continue;
                                }
                                try {
                                    const auto event = json::parse(jsonStr);
                                    const std::string type = event.value("type", "");
                                    if (type == "response.output_text.delta") {
                                        sink.append(event.value("delta", ""));
                                    } else if (type == "error" || type == "response.failed") {
                                        const json err =
                                            event.contains("error")
                                                ? event["error"]
                                                : event.value("response", json::object())
                                                      .value("error", json::object());
                                        sink.fail(err.value("message", "Unknown OpenAI error"));
                                        return false;
                                    }
                                } catch (...) {
                                    // skip malformed json
                                }
                            }
                            lineBuffer = lineBuffer.substr(pos);
                            return true;
                        });

    if (!res) {
        if (!stopToken.stop_requested()) {
            sink.fail("Connection to OpenAI API failed");
        }
    } else if (res->status != 200 && !stopToken.stop_requested()) {
        try {
            const auto errBody = json::parse(res->body);
            const auto errObj = errBody.value("error", json::object());
            sink.fail(std::format("OpenAI API error ({}): {}", res->status,
                                  errObj.value("message", res->body)));
        } catch (...) {
            sink.fail(std::format("OpenAI API error ({}): {}", res->status, res->body));
        }
    }
}
