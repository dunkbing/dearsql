#pragma once

#include "database/lib_database_node.hpp"

class MySQLDatabase;

// one MySQL/MariaDB database; no schema layer, tables and views sit directly under it
class MySQLDatabaseNode final : public LibDatabaseNode {
public:
    ~MySQLDatabaseNode() override {
        waitForLoaders();
    }

    MySQLDatabase* parentDb = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override {
        return name;
    }
    [[nodiscard]] DatabaseType getDatabaseType() const override;

protected:
    dearsql::DatabasePtr openHandle() override;
    [[nodiscard]] std::string fullNamePrefix() const override;
};
