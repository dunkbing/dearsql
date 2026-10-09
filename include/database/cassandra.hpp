#pragma once

#include "cassandra/cassandra_database_node.hpp"
#include "server_database.hpp"

// keyspaces play the part of databases
class CassandraDatabase final : public ServerDatabase<CassandraDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

protected:
    std::unique_ptr<CassandraDatabaseNode> makeNode(const std::string& name) override {
        auto node = std::make_unique<CassandraDatabaseNode>();
        node->name = name;
        node->parentDb = this;
        return node;
    }

    // system keyspaces stay hidden; they can still be queried
    std::vector<std::string> listDatabaseNames() override {
        auto names = ServerDatabase::listDatabaseNames();
        std::erase_if(names, [](const std::string& ks) { return ks.starts_with("system"); });
        return names;
    }
};
