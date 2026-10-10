#pragma once

#include "ai/ai_client.hpp"
#include "database/async_helper.hpp"
#include "database/db.hpp"
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
    // what the prompt needs, copied on the UI thread: the worker never reads the
    // live node (its table vectors are replaced by loaders) or the editor text
    struct PromptInput {
        std::string dbType;
        bool isMongo = false;
        bool tablesLoaded = false;
        std::vector<Table> tables;
        std::vector<std::string> views;
        std::string sql;
    };
    [[nodiscard]] PromptInput snapshotPromptInput() const;
    [[nodiscard]] static std::string buildSystemPrompt(const PromptInput& in,
                                                       std::stop_token stopToken = {});
    [[nodiscard]] static std::string buildSchemaContext(const PromptInput& in,
                                                        std::stop_token stopToken = {});

    IDatabaseNode* node_;
    std::vector<AIChatMessage> messages_;
    std::string currentSQL_;

    [[nodiscard]] std::string dbTypeName() const;

    AsyncOperation<std::string> promptBuilderOp_;
    std::function<void(std::string)> promptReadyCallback_;
};
