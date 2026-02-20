#pragma once

#include "ai/ai_client.hpp"
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

    [[nodiscard]] std::string buildSystemPrompt() const;
    [[nodiscard]] std::string buildSchemaContext() const;
    void setCurrentSQL(const std::string& sql);
    void setDatabaseNode(IDatabaseNode* node);

private:
    IDatabaseNode* node_;
    std::vector<AIChatMessage> messages_;
    std::string currentSQL_;

    [[nodiscard]] std::string dbTypeName() const;
};
