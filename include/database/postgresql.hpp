#pragma once

#include "postgres/postgres_database_node.hpp"
#include "server_database.hpp"

// PostgreSQL and Redshift server
class PostgresDatabase final : public ServerDatabase<PostgresDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

    // dropping the connected database moves the connection to the maintenance one
    std::pair<bool, std::string> dropDatabase(const std::string& name) override {
        if (name != connectionInfo.database)
            return ServerDatabase::dropDatabase(name);
        const std::string previous = connectionInfo.database;
        connectionInfo.database =
            connectionInfo.type == DatabaseType::REDSHIFT ? "dev" : "postgres";
        auto status = ServerDatabase::dropDatabase(name);
        if (!status.first)
            connectionInfo.database = previous;
        return status;
    }

protected:
    std::unique_ptr<PostgresDatabaseNode> makeNode(const std::string& name) override {
        auto node = std::make_unique<PostgresDatabaseNode>();
        node->name = name;
        node->parentDb = this;
        return node;
    }

    void onRefreshed(PostgresDatabaseNode& node) override {
        node.startSchemasLoadAsync(true, true);
    }
};
