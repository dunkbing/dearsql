#include "database/mysql.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct MySQLConfig {
        std::string name = "MySQLIntegration";
        std::string host;
        int port = 3306;
        std::string database;
        std::string user;
        std::string password;
        bool showAllDatabases = false;
    };

    std::optional<MySQLConfig> loadMySQLConfigFromEnv() {
        MySQLConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_MYSQL_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_MYSQL_PORT");
        const char* databaseEnv = std::getenv("DEARSQL_TEST_MYSQL_DB");
        const char* userEnv = std::getenv("DEARSQL_TEST_MYSQL_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_MYSQL_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_MYSQL_NAME");

        if (!hostEnv || !databaseEnv || !userEnv) {
            return std::nullopt;
        }

        cfg.host = hostEnv;
        cfg.database = databaseEnv;
        cfg.user = userEnv;
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

class MySQLDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadMySQLConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing MySQL test configuration. Run scripts/run_tests to launch "
            << "Docker test databases automatically, or set DEARSQL_TEST_MYSQL_HOST / "
            << "DEARSQL_TEST_MYSQL_DB / DEARSQL_TEST_MYSQL_USER (optional PORT/PASSWORD/NAME).";

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::MYSQL;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.database;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = config.showAllDatabases;

        database = std::make_shared<MySQLDatabase>(connInfo);
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
        ASSERT_TRUE(connected) << "MySQL connection failed: " << lastError;
        tableName = TestHelpers::makeUniqueIdentifier("dearsql_mysql_test_");
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
        if (database && !tableName.empty()) {
            database->executeQuery(std::format("DROP TABLE IF EXISTS `{}`", tableName));
        }
    }

    MySQLConfig config;
    std::shared_ptr<MySQLDatabase> database;
    std::string tableName;
};

TEST_F(MySQLDatabaseIntegrationTest, ExecuteQueryStructuredReadsInsertedRows) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format(
        "CREATE TABLE `{}` (id INT PRIMARY KEY AUTO_INCREMENT, value TEXT NOT NULL)", tableName));
    auto createSuccess = !r1.empty() && r1[0].success;
    auto createError = r1.empty() ? std::string("No result") : r1[0].errorMessage;
    ASSERT_TRUE(createSuccess) << createError;

    auto r2 = database->executeQuery(
        std::format("INSERT INTO `{}`(value) VALUES ('delta'), ('epsilon'), ('zeta')", tableName));
    auto insertSuccess = !r2.empty() && r2[0].success;
    auto insertError = r2.empty() ? std::string("No result") : r2[0].errorMessage;
    ASSERT_TRUE(insertSuccess) << insertError;

    auto results =
        database->executeQuery(std::format("SELECT value FROM `{}` ORDER BY id", tableName));
    ASSERT_FALSE(results.empty());
    auto& result = results[0];
    ASSERT_TRUE(result.success) << result.errorMessage;

    ASSERT_EQ(result.columnNames.size(), 1u);
    EXPECT_EQ(result.columnNames[0], "value");

    ASSERT_EQ(result.tableData.size(), 3u);
    EXPECT_EQ(result.tableData[0][0], "delta");
    EXPECT_EQ(result.tableData[1][0], "epsilon");
    EXPECT_EQ(result.tableData[2][0], "zeta");
}

TEST_F(MySQLDatabaseIntegrationTest, DropCurrentlyConnectedDatabaseSwitchesToMysqlDatabase) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_mysql_drop_active_");

    auto [created, createErr] = database->createDatabase(tempDb);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("Access denied") != std::string::npos ||
            err.find("denied") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required for this test. Error: "
                         << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    DatabaseConnectionInfo tempConnInfo = database->getConnectionInfo();
    tempConnInfo.database = tempDb;
    auto activeDb = std::make_shared<MySQLDatabase>(tempConnInfo);

    const auto [connected, connectErr] = activeDb->connect();
    ASSERT_TRUE(connected) << connectErr;

    const auto [dropped, dropErr] = activeDb->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
    EXPECT_EQ(activeDb->getConnectionInfo().database, "mysql");

    auto verifyResult = database->executeQuery(std::format("SHOW DATABASES LIKE '{}'", tempDb));
    ASSERT_FALSE(verifyResult.empty());
    ASSERT_TRUE(verifyResult[0].success) << verifyResult[0].errorMessage;
    EXPECT_TRUE(verifyResult[0].tableData.empty());
}

TEST_F(MySQLDatabaseIntegrationTest,
       CreateDatabaseWithOptionsCreatesDatabaseWithCharsetAndCollation) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_mysql_create_opts_");

    CreateDatabaseOptions options;
    options.name = tempDb;
    options.charset = "utf8mb4";
    options.collation = "utf8mb4_unicode_ci";
    options.comment = "dearsql option test's comment";

    auto [created, createErr] = database->createDatabaseWithOptions(options);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("Access denied") != std::string::npos ||
            err.find("denied") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required for this test. Error: "
                         << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    auto result = database->executeQuery(
        std::format("SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME "
                    "FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = '{}'",
                    tempDb));
    ASSERT_FALSE(result.empty());
    ASSERT_TRUE(result[0].success) << result[0].errorMessage;
    ASSERT_EQ(result[0].tableData.size(), 1u);
    EXPECT_EQ(result[0].tableData[0][0], options.charset);
    EXPECT_EQ(result[0].tableData[0][1], options.collation);

    const auto [dropped, dropErr] = database->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
}
