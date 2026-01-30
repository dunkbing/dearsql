#include "database/redis.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct RedisConfig {
        std::string name = "RedisIntegration";
        std::string host;
        int port = 6379;
    };

    std::optional<RedisConfig> loadRedisConfigFromEnv() {
        RedisConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_REDIS_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_REDIS_PORT");
        const char* nameEnv = std::getenv("DEARSQL_TEST_REDIS_NAME");

        if (!hostEnv) {
            return std::nullopt;
        }

        cfg.host = hostEnv;
        if (nameEnv && *nameEnv != '\0') {
            cfg.name = nameEnv;
        }
        if (portEnv && *portEnv != '\0') {
            cfg.port = std::stoi(portEnv);
        }

        return cfg;
    }
} // namespace

class RedisDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadRedisConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing Redis test configuration. Run scripts/run_tests to launch "
            << "Docker test databases automatically, or set DEARSQL_TEST_REDIS_HOST "
            << "(optional PORT/NAME).";

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::REDIS;
        connInfo.host = config.host;
        connInfo.port = config.port;

        database = std::make_shared<RedisDatabase>(connInfo);

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
        ASSERT_TRUE(connected) << "Redis connection failed: " << lastError;

        testKeyPrefix = TestHelpers::makeUniqueIdentifier("dearsql_test_");
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
        if (database && !testKeyPrefix.empty()) {
            // Delete test keys
            database->executeQuery("DEL " + testKeyPrefix + "string");
            database->executeQuery("DEL " + testKeyPrefix + "list");
            database->executeQuery("DEL " + testKeyPrefix + "hash");
            database->executeQuery("DEL " + testKeyPrefix + "set");
        }
    }

    RedisConfig config;
    std::shared_ptr<RedisDatabase> database;
    std::string testKeyPrefix;
};

TEST_F(RedisDatabaseIntegrationTest, ConnectsToRedis) {
    EXPECT_TRUE(database->isConnected());
}

TEST_F(RedisDatabaseIntegrationTest, SetAndGetStringKey) {
    ASSERT_NE(database, nullptr);

    std::string key = testKeyPrefix + "string";

    auto [success, error] = database->executeQuery("SET " + key + " \"hello world\"");
    ASSERT_TRUE(success) << error;

    std::string value = database->getKeyValue(key);
    EXPECT_EQ(value, "hello world");

    std::string keyType = database->getKeyType(key);
    EXPECT_EQ(keyType, "string");
}

TEST_F(RedisDatabaseIntegrationTest, ListOperations) {
    ASSERT_NE(database, nullptr);

    std::string key = testKeyPrefix + "list";

    auto [success1, error1] = database->executeQuery("RPUSH " + key + " one two three");
    ASSERT_TRUE(success1) << error1;

    std::string keyType = database->getKeyType(key);
    EXPECT_EQ(keyType, "list");

    auto result = database->executeQueryWithResult("LRANGE " + key + " 0 -1");
    EXPECT_TRUE(result.success);
    // Redis returns list elements - format depends on implementation
    EXPECT_FALSE(result.tableData.empty());
}

TEST_F(RedisDatabaseIntegrationTest, HashOperations) {
    ASSERT_NE(database, nullptr);

    std::string key = testKeyPrefix + "hash";

    auto [success1, error1] =
        database->executeQuery("HSET " + key + " field1 value1 field2 value2");
    ASSERT_TRUE(success1) << error1;

    std::string keyType = database->getKeyType(key);
    EXPECT_EQ(keyType, "hash");

    auto result = database->executeQueryWithResult("HGETALL " + key);
    EXPECT_TRUE(result.success);
}

TEST_F(RedisDatabaseIntegrationTest, SetOperations) {
    ASSERT_NE(database, nullptr);

    std::string key = testKeyPrefix + "set";

    auto [success1, error1] = database->executeQuery("SADD " + key + " member1 member2 member3");
    ASSERT_TRUE(success1) << error1;

    std::string keyType = database->getKeyType(key);
    EXPECT_EQ(keyType, "set");

    auto result = database->executeQueryWithResult("SMEMBERS " + key);
    EXPECT_TRUE(result.success);
}

TEST_F(RedisDatabaseIntegrationTest, KeyTTL) {
    ASSERT_NE(database, nullptr);

    std::string key = testKeyPrefix + "string";

    auto [success1, error1] = database->executeQuery("SET " + key + " \"expiring\"");
    ASSERT_TRUE(success1) << error1;

    // Key without TTL should return -1
    int64_t ttl = database->getKeyTTL(key);
    EXPECT_EQ(ttl, -1);

    // Set TTL
    auto [success2, error2] = database->executeQuery("EXPIRE " + key + " 3600");
    ASSERT_TRUE(success2) << error2;

    ttl = database->getKeyTTL(key);
    EXPECT_GT(ttl, 0);
    EXPECT_LE(ttl, 3600);
}

TEST_F(RedisDatabaseIntegrationTest, GetKeys) {
    ASSERT_NE(database, nullptr);

    std::string key1 = testKeyPrefix + "string";
    std::string key2 = testKeyPrefix + "list";

    database->executeQuery("SET " + key1 + " \"value1\"");
    database->executeQuery("RPUSH " + key2 + " item1");

    auto keys = database->getKeys(testKeyPrefix + "*");
    EXPECT_GE(keys.size(), 2u);

    bool foundString = false;
    bool foundList = false;
    for (const auto& k : keys) {
        if (k.name == key1)
            foundString = true;
        if (k.name == key2)
            foundList = true;
    }
    EXPECT_TRUE(foundString);
    EXPECT_TRUE(foundList);
}

TEST_F(RedisDatabaseIntegrationTest, PingCommand) {
    ASSERT_NE(database, nullptr);

    auto result = database->executeQueryWithResult("PING");
    EXPECT_TRUE(result.success);
}
