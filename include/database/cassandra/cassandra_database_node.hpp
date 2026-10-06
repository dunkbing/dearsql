#pragma once

#include "database/lib_database_node.hpp"

class CassandraDatabase;

// one keyspace; "views" are its materialized views
class CassandraDatabaseNode final : public LibDatabaseNode {
public:
    ~CassandraDatabaseNode() override {
        waitForLoaders();
    }

    CassandraDatabase* parentDb = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override {
        return name;
    }
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::CASSANDRA;
    }

protected:
    dearsql::DatabasePtr openHandle() override;
    [[nodiscard]] std::string fullNamePrefix() const override;
    // one driver session, serialized inside the library
    [[nodiscard]] size_t poolSize() const override {
        return 0;
    }
};
