#include "database/mongodb.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct MongoDBConfig {
        std::string name = "MongoDBIntegration";
        std::string host;
        int port = 27017;
        std::string database;
        std::string user;
        std::string password;
        bool showAllDatabases = false;
    };

    std::optional<MongoDBConfig> loadMongoDBConfigFromEnv() {
        MongoDBConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_MONGODB_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_MONGODB_PORT");
        const char* databaseEnv = std::getenv("DEARSQL_TEST_MONGODB_DB");
        const char* userEnv = std::getenv("DEARSQL_TEST_MONGODB_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_MONGODB_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_MONGODB_NAME");

        if (!hostEnv) {
            return std::nullopt;
        }

        cfg.host = hostEnv;
        if (databaseEnv) {
            cfg.database = databaseEnv;
        }
        if (userEnv) {
            cfg.user = userEnv;
        }
        if (passwordEnv) {
            cfg.password = passwordEnv;
        }
        if (nameEnv && *nameEnv != '\0') {
            cfg.name = nameEnv;
        }
        if (portEnv && *portEnv != '\0') {
            cfg.port = std::stoi(portEnv);
        }

        return cfg;
    }
} // namespace

class MongoDBDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadMongoDBConfigFromEnv();
        if (!cfgOpt.has_value()) {
            GTEST_SKIP() << "Skipping MongoDB tests: DEARSQL_TEST_MONGODB_HOST not set. "
                         << "Run scripts/run_tests to launch Docker test databases.";
        }

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::MONGODB;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.database.empty() ? "test" : config.database;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = config.showAllDatabases;

        database = std::make_shared<MongoDBDatabase>(connInfo);
        bool connected = false;
        std::string lastError;
        for (int attempt = 0; attempt < 30; ++attempt) {
            const auto [success, error] = database->connect();
            if (success) {
                connected = true;
                break;
            }
            lastError = error;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ASSERT_TRUE(connected) << "MongoDB connection failed: " << lastError;
        collectionName = TestHelpers::makeUniqueIdentifier("dearsql_mongo_test_");
        cleanup();
    }

    void TearDown() override {
        cleanup();
        if (database) {
            database->disconnect();
            database.reset();
        }
    }

    void cleanup() {
        if (database && !collectionName.empty()) {
            // Drop test collection
            std::string dropQuery = std::format(
                R"({{"database": "test", "collection": "{}", "command": "dropCollection"}})",
                collectionName);
            database->executeQuery(dropQuery);
        }
    }

    MongoDBConfig config;
    std::shared_ptr<MongoDBDatabase> database;
    std::string collectionName;
};

TEST_F(MongoDBDatabaseIntegrationTest, ConnectSuccessfully) {
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(database->isConnected());
}

TEST_F(MongoDBDatabaseIntegrationTest, ListDatabases) {
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(database->isConnected());

    // Force load databases
    database->refreshDatabaseNames();

    // Wait for async load to complete
    for (int i = 0; i < 30 && database->isLoadingDatabases(); ++i) {
        database->checkDatabasesStatusAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(database->areDatabasesLoaded());
}

TEST_F(MongoDBDatabaseIntegrationTest, InsertAndFindDocuments) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(collectionName.empty());

    // Create collection by inserting a document
    std::string insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "test1", "value": 42}}}})",
        collectionName);
    auto [insertSuccess, insertError] = database->executeQuery(insertQuery);
    ASSERT_TRUE(insertSuccess) << insertError;

    // Insert another document
    insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "test2", "value": 100}}}})",
        collectionName);
    auto [insertSuccess2, insertError2] = database->executeQuery(insertQuery);
    ASSERT_TRUE(insertSuccess2) << insertError2;

    // Find documents
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{}}}})",
        collectionName);
    auto results = database->executeQueryWithResult(findQuery, 100);
    ASSERT_FALSE(results.empty());
    auto& result = results[0];
    ASSERT_TRUE(result.success) << result.errorMessage;

    EXPECT_EQ(result.tableData.size(), 2u);
}

TEST_F(MongoDBDatabaseIntegrationTest, UpdateDocuments) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(collectionName.empty());

    // Insert a document
    std::string insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "update_test", "value": 1}}}})",
        collectionName);
    auto [insertSuccess, insertError] = database->executeQuery(insertQuery);
    ASSERT_TRUE(insertSuccess) << insertError;

    // Update the document
    std::string updateQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "update", "filter": {{"name": "update_test"}}, "update": {{"$set": {{"value": 999}}}}}})",
        collectionName);
    auto [updateSuccess, updateError] = database->executeQuery(updateQuery);
    ASSERT_TRUE(updateSuccess) << updateError;

    // Verify update
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{"name": "update_test"}}}})",
        collectionName);
    auto results = database->executeQueryWithResult(findQuery, 100);
    ASSERT_FALSE(results.empty());
    auto& result = results[0];
    ASSERT_TRUE(result.success) << result.errorMessage;

    ASSERT_EQ(result.tableData.size(), 1u);
    // The document JSON should contain "value": 999
    EXPECT_NE(result.tableData[0][1].find("999"), std::string::npos);
}

TEST_F(MongoDBDatabaseIntegrationTest, DeleteDocuments) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(collectionName.empty());

    // Insert documents
    std::string insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "delete_me"}}}})",
        collectionName);
    database->executeQuery(insertQuery);

    // Delete
    std::string deleteQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "delete", "filter": {{"name": "delete_me"}}}})",
        collectionName);
    auto [deleteSuccess, deleteError] = database->executeQuery(deleteQuery);
    ASSERT_TRUE(deleteSuccess) << deleteError;

    // Verify deletion
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{"name": "delete_me"}}}})",
        collectionName);
    auto results = database->executeQueryWithResult(findQuery, 100);
    ASSERT_FALSE(results.empty());
    auto& result = results[0];
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.tableData.size(), 0u);
}
