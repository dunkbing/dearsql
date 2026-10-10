#pragma once

#include "postgres/postgres_database_node.hpp"
#include "server_database.hpp"

// PostgreSQL and Redshift server
class PostgresDatabase final : public ServerDatabase<PostgresDatabaseNode> {
public:
    using ServerDatabase::ServerDatabase;

protected:
    // dropping the connected database moves the connection to the maintenance one
    std::string databaseAfterDrop(const std::string&, const std::string&) override {
        return connectionInfo.type == DatabaseType::REDSHIFT ? "dev" : "postgres";
    }

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
