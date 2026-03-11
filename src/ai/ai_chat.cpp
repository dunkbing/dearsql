#include "ai/ai_chat.hpp"
#include "database/database_node.hpp"
#include <format>

AIChatState::AIChatState(IDatabaseNode* node) : node_(node) {}

void AIChatState::addUserMessage(const std::string& content) {
    messages_.push_back({"user", content});
}

void AIChatState::startAssistantMessage() {
    messages_.push_back({"assistant", ""});
}

void AIChatState::appendToAssistant(const std::string& delta) {
    if (!messages_.empty() && messages_.back().role == "assistant") {
        messages_.back().content += delta;
    }
}

void AIChatState::finalizeAssistant() {
    // Trim trailing whitespace from the last assistant message
    if (!messages_.empty() && messages_.back().role == "assistant") {
        auto& content = messages_.back().content;
        while (!content.empty() && (content.back() == ' ' || content.back() == '\n')) {
            content.pop_back();
        }
    }
}

const std::vector<AIChatMessage>& AIChatState::getMessages() const {
    return messages_;
}

void AIChatState::clear() {
    messages_.clear();
}

void AIChatState::setCurrentSQL(const std::string& sql) {
    currentSQL_ = sql;
}

void AIChatState::setDatabaseNode(IDatabaseNode* node) {
    node_ = node;
}

std::string AIChatState::dbTypeName() const {
    if (!node_) {
        return "SQL";
    }
    switch (node_->getDatabaseType()) {
    case DatabaseType::SQLITE:
        return "SQLite";
    case DatabaseType::POSTGRESQL:
        return "PostgreSQL";
    case DatabaseType::MYSQL:
        return "MySQL";
    case DatabaseType::MARIADB:
        return "MariaDB";
    case DatabaseType::MONGODB:
        return "MongoDB";
    case DatabaseType::REDIS:
        return "Redis";
    case DatabaseType::MSSQL:
        return "MSSQL";
    case DatabaseType::REDSHIFT:
        return "Redshift";
    }
    return "SQL";
}

std::string AIChatState::buildSchemaContext() const {
    if (!node_ || !node_->isTablesLoaded()) {
        return "(schema not loaded)";
    }

    const bool isMongo = node_->getDatabaseType() == DatabaseType::MONGODB;
    std::string ctx;
    for (const auto& table : node_->getTables()) {
        if (isMongo) {
            ctx += std::format("Collection: {}", table.name);
            if (!table.columns.empty()) {
                ctx += " (fields: ";
                for (size_t i = 0; i < table.columns.size(); ++i) {
                    if (i > 0)
                        ctx += ", ";
                    ctx += table.columns[i].name;
                }
                ctx += ")";
            }
            ctx += "\n";
        } else {
            ctx += std::format("Table: {} (", table.name);
            for (size_t i = 0; i < table.columns.size(); ++i) {
                if (i > 0)
                    ctx += ", ";
                const auto& col = table.columns[i];
                ctx += col.name + " " + col.type;
                if (col.isPrimaryKey)
                    ctx += " PK";
                if (col.isNotNull)
                    ctx += " NOT NULL";
            }
            ctx += ")\n";

            for (const auto& fk : table.foreignKeys) {
                ctx += std::format("  FK: {}.{} -> {}.{}\n", table.name, fk.sourceColumn,
                                   fk.targetTable, fk.targetColumn);
            }
        }
    }

    if (node_->isViewsLoaded()) {
        for (const auto& view : node_->getViews()) {
            ctx += std::format("View: {}\n", view.name);
        }
    }

    return ctx;
}

std::string AIChatState::buildSystemPrompt() const {
    std::string dbType = dbTypeName();
    std::string schema = buildSchemaContext();
    bool isMongo = node_ && node_->getDatabaseType() == DatabaseType::MONGODB;

    if (isMongo) {
        return std::format("You are a MongoDB query assistant.\n"
                           "Generate valid JSON queries for MongoDB. The query format is:\n"
                           "{{\"collection\": \"name\", \"command\": \"find\", \"filter\": {{}}}}\n"
                           "Supported commands: find, aggregate, insert, update, delete, "
                           "createCollection, dropCollection, runCommand.\n"
                           "Put queries in ```json code blocks. Be concise.\n\n"
                           "Current query in editor:\n{}\n\n"
                           "Collections:\n{}",
                           currentSQL_.empty() ? "// empty" : currentSQL_, schema);
    }

    return std::format("You are a SQL assistant for a {} database.\n"
                       "Generate valid SQL for {}. Use exact table/column names from the schema.\n"
                       "Put SQL in ```sql code blocks. Be concise.\n\n"
                       "Current SQL in editor:\n{}\n\n"
                       "Schema:\n{}",
                       dbType, dbType, currentSQL_.empty() ? "-- empty" : currentSQL_, schema);
}
