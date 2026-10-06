#pragma once

#include "database/schema_database_node.hpp"

class MSSQLDatabaseNode;

// one schema of a SQL Server database; runs on the database node's pooled handles
class MSSQLSchemaNode final : public LibSchemaNode {
public:
    ~MSSQLSchemaNode() override {
        waitForLoaders();
    }

    MSSQLDatabaseNode* parentDbNode = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override;
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::MSSQL;
    }

protected:
    [[nodiscard]] LibDatabaseNode* databaseNode() const override;
    [[nodiscard]] std::string fullNamePrefix() const override;
};
