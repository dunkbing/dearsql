#pragma once

#include "mssql_schema_node.hpp"

class MSSQLDatabase;

// one SQL Server database: pooled handles plus a node per schema
class MSSQLDatabaseNode final : public SchemaDatabaseNode<MSSQLSchemaNode> {
public:
    ~MSSQLDatabaseNode() override {
        waitForLoaders();
    }

    MSSQLDatabase* parentDb = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override {
        return name;
    }
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::MSSQL;
    }

protected:
    dearsql::DatabasePtr openHandle() override;
    std::unique_ptr<MSSQLSchemaNode> makeSchema(const std::string& schemaName) override;
};
