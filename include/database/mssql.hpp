#pragma once

#include "mssql/mssql_database_node.hpp"
#include "server_database.hpp"

// SQL Server; databases hold schemas, which hold tables, views and routines
class MSSQLDatabase final : public ServerDatabase<MSSQLDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

    // dropping the connected database moves the connection to master
    std::pair<bool, std::string> dropDatabase(const std::string& name) override {
        const bool current = name == connectionInfo.database;
        auto status = ServerDatabase::dropDatabase(name);
        if (status.first && current)
            connectionInfo.database = "master";
        return status;
    }

protected:
    std::unique_ptr<MSSQLDatabaseNode> makeNode(const std::string& name) override {
        auto node = std::make_unique<MSSQLDatabaseNode>();
        node->name = name;
        node->parentDb = this;
        return node;
    }
};
