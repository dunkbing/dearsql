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
    auto r1 = database->executeQuery(insertQuery);
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    // Insert another document
    insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "test2", "value": 100}}}})",
        collectionName);
    auto r2 = database->executeQuery(insertQuery);
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    // Find documents
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{}}}})",
        collectionName);
    auto result = database->executeQuery(findQuery, 100);
    ASSERT_FALSE(result.empty());
    ASSERT_TRUE(result[0].success) << result[0].errorMessage;

    EXPECT_EQ(result[0].tableData.size(), 2u);
}

TEST_F(MongoDBDatabaseIntegrationTest, UpdateDocuments) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(collectionName.empty());

    // Insert a document
    std::string insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "update_test", "value": 1}}}})",
        collectionName);
    auto r1 = database->executeQuery(insertQuery);
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    // Update the document
    std::string updateQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "update", "filter": {{"name": "update_test"}}, "update": {{"$set": {{"value": 999}}}}}})",
        collectionName);
    auto r2 = database->executeQuery(updateQuery);
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    // Verify update
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{"name": "update_test"}}}})",
        collectionName);
    auto result = database->executeQuery(findQuery, 100);
    ASSERT_FALSE(result.empty());
    ASSERT_TRUE(result[0].success) << result[0].errorMessage;

    ASSERT_EQ(result[0].tableData.size(), 1u);
    // The document JSON should contain "value": 999
    EXPECT_NE(result[0].tableData[0][1].find("999"), std::string::npos);
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
    auto r1 = database->executeQuery(deleteQuery);
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    // Verify deletion
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{"name": "delete_me"}}}})",
        collectionName);
    auto result = database->executeQuery(findQuery, 100);
    ASSERT_FALSE(result.empty());
    ASSERT_TRUE(result[0].success) << result[0].errorMessage;
    EXPECT_EQ(result[0].tableData.size(), 0u);
}

// ========== Database Node DDL Tests ==========

TEST_F(MongoDBDatabaseIntegrationTest, DropCollectionRemovesCollection) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(collectionName.empty());

    // Create collection by inserting a document
    std::string insertQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "insert", "document": {{"name": "drop_test"}}}})",
        collectionName);
    auto r = database->executeQuery(insertQuery);
    ASSERT_TRUE(r.success()) << r.errorMessage();

    // Get the database node and call dropCollection
    auto* dbNode = database->getDatabaseData("test");
    ASSERT_NE(dbNode, nullptr);

    auto [ok, err] = dbNode->dropTable(collectionName);
    ASSERT_TRUE(ok) << err;

    // Wait for async collections reload triggered by dropCollection
    for (int i = 0; i < 50 && dbNode->isLoadingTables(); ++i) {
        dbNode->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(dbNode->isLoadingTables()) << "Async collections reload did not complete in time";

    // Verify collection is gone by trying to find documents
    std::string findQuery = std::format(
        R"({{"database": "test", "collection": "{}", "command": "find", "filter": {{}}}})",
        collectionName);
    auto result = database->executeQuery(findQuery, 100);
    ASSERT_FALSE(result.empty());
    EXPECT_TRUE(result[0].tableData.empty());
}

// pins the json and cell text for every bson type the ui shows
TEST_F(MongoDBDatabaseIntegrationTest, JsonShapeOfEveryType) {
    ASSERT_NE(database, nullptr);
    const std::string doc =
        R"({"_id": {"$oid": "65a1b2c3d4e5f60718293a4b"}, "s": "hi", "i": 7, "l": {"$numberLong": "5000000000"}, "f": 1.5, "g": 2.0, "dec": {"$numberDecimal": "1.25"}, "d": {"$date": {"$numberLong": "1700000000123"}}, "t": true, "n": null, "sub": {"a": 1, "b": [1, "x"]}, "arr": [1, "x", {"y": 2}], "bin": {"$binary": {"base64": "AQI=", "subType": "00"}}, "re": {"$regularExpression": {"pattern": "ab", "options": "i"}}, "ts": {"$timestamp": {"t": 1, "i": 2}}})";
    auto ins = database->executeQuery(
        std::format(R"({{"insert": "{}", "documents": [{}]}})", collectionName, doc));
    ASSERT_TRUE(ins.success()) << ins.errorMessage();

    auto find = database->executeQuery(
        std::format(R"({{"database": "test", "collection": "{}", "command": "find"}})",
                    collectionName),
        100);
    ASSERT_TRUE(find.success()) << find.errorMessage();
    ASSERT_EQ(find[0].tableData.size(), 1u);
    auto agg = database->executeQuery(
        std::format(
            R"({{"database": "test", "collection": "{}", "command": "aggregate", "pipeline": [{{"$project": {{"arr": 1}}}}]}})",
            collectionName),
        100);
    ASSERT_TRUE(agg.success()) << agg.errorMessage();
    ASSERT_EQ(agg[0].tableData.size(), 1u);

    auto lib = database->libConnection()->database("test");
    ASSERT_TRUE(lib);
    dearsql::Table t = lib->describeTable(collectionName);
    std::string types;
    for (const auto& c : t.columns)
        types += c.name + ":" + c.type + (c.isPrimaryKey ? "*" : "") + " ";
    auto rows = lib->getTableData(t, 10, 0, "", "");
    ASSERT_EQ(rows.size(), 1u);
    std::string cells;
    for (const auto& c : rows[0])
        cells += c + "\n";

    // legacy extended json, as before the move to the c driver
    EXPECT_EQ(find[0].tableData[0][0], R"({ "_id" : { "$oid" : "65a1b2c3d4e5f60718293a4b" } })");
    EXPECT_EQ(
        find[0].tableData[0][1],
        R"({ "_id" : { "$oid" : "65a1b2c3d4e5f60718293a4b" }, "s" : "hi", "i" : 7, "l" : 5000000000, "f" : 1.5, "g" : 2.0, "dec" : { "$numberDecimal" : "1.25" }, "d" : { "$date" : 1700000000123 }, "t" : true, "n" : null, "sub" : { "a" : 1, "b" : [ 1, "x" ] }, "arr" : [ 1, "x", { "y" : 2 } ], "bin" : { "$binary" : "AQI=", "$type" : "00" }, "re" : { "$regex" : "ab", "$options" : "i" }, "ts" : { "$timestamp" : { "t" : 1, "i" : 2 } } })");
    EXPECT_EQ(
        agg[0].tableData[0][0],
        R"({ "_id" : { "$oid" : "65a1b2c3d4e5f60718293a4b" }, "arr" : [ 1, "x", { "y" : 2 } ] })");
    EXPECT_EQ(types, "_id:objectId* arr:array bin:binary d:date dec:decimal128 f:double g:double "
                     "i:int32 l:int64 n:null re:unknown s:string sub:object t:bool ts:unknown ");
    EXPECT_EQ(cells, R"(65a1b2c3d4e5f60718293a4b
[ 1, "x", { "y" : 2 } ]
{ "v" : { "$binary" : "AQI=", "$type" : "00" } }
2023-11-14 22:13:20
1.25
1.500000
2.000000
7
5000000000
dearsql__null
{ "v" : { "$regex" : "ab", "$options" : "i" } }
hi
{ "a" : 1, "b" : [ 1, "x" ] }
dearsql__bool_true
{ "v" : { "$timestamp" : { "t" : 1, "i" : 2 } } }
)");
}
