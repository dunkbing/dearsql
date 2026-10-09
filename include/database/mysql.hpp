#pragma once

#include "mysql/mysql_database_node.hpp"
#include "server_database.hpp"

// MySQL and MariaDB server
class MySQLDatabase final : public ServerDatabase<MySQLDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

protected:
    std::unique_ptr<MySQLDatabaseNode> makeNode(const std::string& name) override {
        auto node = std::make_unique<MySQLDatabaseNode>();
        node->name = name;
        node->parentDb = this;
        return node;
    }
};
