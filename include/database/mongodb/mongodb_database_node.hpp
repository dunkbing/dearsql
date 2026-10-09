#pragma once

#include "database/lib_database_node.hpp"

class MongoDBDatabase;

// one MongoDB database; collections are its "tables"
class MongoDBDatabaseNode final : public LibDatabaseNode {
public:
    ~MongoDBDatabaseNode() override {
        waitForLoaders();
    }

    MongoDBDatabase* parentDb = nullptr;

    [[nodiscard]] DatabaseInterface* ownerDatabase() const override;
    [[nodiscard]] std::string getFullPath() const override {
        return name;
    }
    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::MONGODB;
    }

protected:
    dearsql::DatabasePtr openHandle() override;
    [[nodiscard]] std::string fullNamePrefix() const override;
    // the driver's mongocxx::pool is already thread-safe
    [[nodiscard]] size_t poolSize() const override {
        return 0;
    }
};
