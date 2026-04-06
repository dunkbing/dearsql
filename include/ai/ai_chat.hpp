#pragma once

#include "ai/ai_client.hpp"
#include "database/async_helper.hpp"
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

class IDatabaseNode;

class AIChatState {
public:
    explicit AIChatState(IDatabaseNode* node);

    void addUserMessage(const std::string& content);
    void startAssistantMessage();
    void appendToAssistant(const std::string& delta);
    void finalizeAssistant();
    [[nodiscard]] const std::vector<AIChatMessage>& getMessages() const;
    void clear();

    void setCurrentSQL(const std::string& sql);
    void setDatabaseNode(IDatabaseNode* node);

    void buildSystemPromptAsync(std::function<void(std::string)> callback);
    void cancelAsyncPrompt();
    void pollAsyncPrompt();
    [[nodiscard]] bool isBuildingPrompt() const;

private:
    [[nodiscard]] std::string buildSystemPrompt(std::stop_token stopToken = {}) const;
    [[nodiscard]] std::string buildSchemaContext(std::stop_token stopToken = {}) const;

    IDatabaseNode* node_;
    std::vector<AIChatMessage> messages_;
    std::string currentSQL_;

    [[nodiscard]] std::string dbTypeName() const;

    AsyncOperation<std::string> promptBuilderOp_;
    std::function<void(std::string)> promptReadyCallback_;
};
