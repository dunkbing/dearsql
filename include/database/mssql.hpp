#pragma once

#include "mssql/mssql_database_node.hpp"
#include "server_database.hpp"

// SQL Server; databases hold schemas, which hold tables, views and routines
class MSSQLDatabase final : public ServerDatabase<MSSQLDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

protected:
    // dropping the connected database moves the connection to master
    std::string databaseAfterDrop(const std::string&, const std::string&) override {
        return "master";
    }

    std::unique_ptr<MSSQLDatabaseNode> makeNode(const std::string& name) override {
        auto node = std::make_unique<MSSQLDatabaseNode>();
        node->name = name;
        node->parentDb = this;
        return node;
    }
};
