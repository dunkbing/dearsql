#pragma once

#include "database/schema_database_node.hpp"

class PostgresDatabase;
class PostgresDatabaseNode;

// one schema; runs on its database node's pooled handles
class PostgresSchemaNode final : public LibSchemaNode {
public:
    ~PostgresSchemaNode() override {
        waitForLoaders();
    }

    PostgresDatabaseNode* parentDbNode = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override;
    [[nodiscard]] DatabaseType getDatabaseType() const override;

    // a forced reload also refreshes loaded materialized views and sequences
    void startTablesLoadAsync(bool force = false) override;

    std::pair<bool, std::string> renameSchema(const std::string& newName);
    std::pair<bool, std::string> dropSchema();

protected:
    [[nodiscard]] LibDatabaseNode* databaseNode() const override;
};

// one database of the server: pooled handles plus its schemas
class PostgresDatabaseNode final : public SchemaDatabaseNode<PostgresSchemaNode> {
public:
    ~PostgresDatabaseNode() override {
        waitForLoaders();
    }

    PostgresDatabase* parentDb = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override {
        return name;
    }
    [[nodiscard]] DatabaseType getDatabaseType() const override;

protected:
    dearsql::DatabasePtr openHandle() override;
    std::unique_ptr<PostgresSchemaNode> makeSchema(const std::string& schemaName) override;
};
