#pragma once

#include "database/lib_database_node.hpp"

class OracleDatabase;

// one schema; its pooled sessions run with CURRENT_SCHEMA set to it
class OracleDatabaseNode final : public LibDatabaseNode {
public:
    ~OracleDatabaseNode() override {
        waitForLoaders();
    }

    OracleDatabase* parentDb = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override {
        return name;
    }
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::ORACLE;
    }

protected:
    dearsql::DatabasePtr openHandle() override;
    [[nodiscard]] std::string fullNamePrefix() const override;
};
