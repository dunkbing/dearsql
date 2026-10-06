#pragma once

#include "mongodb/mongodb_database_node.hpp"
#include "server_database.hpp"

class MongoDBDatabase final : public ServerDatabase<MongoDBDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

protected:
    std::unique_ptr<MongoDBDatabaseNode> makeNode(const std::string& name) override {
        auto node = std::make_unique<MongoDBDatabaseNode>();
        node->name = name;
        node->parentDb = this;
        return node;
    }
};
