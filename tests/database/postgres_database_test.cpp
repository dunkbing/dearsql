#include "database/postgresql.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct PostgresConfig {
        std::string name = "PostgresIntegration";
        std::string host;
        int port = 5432;
        std::string database;
        std::string user;
        std::string password;
    };

    std::optional<PostgresConfig> loadPostgresConfigFromEnv() {
        PostgresConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_PG_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_PG_PORT");
        const char* databaseEnv = std::getenv("DEARSQL_TEST_PG_DB");
        const char* userEnv = std::getenv("DEARSQL_TEST_PG_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_PG_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_PG_NAME");

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

class PostgresDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadPostgresConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing PostgreSQL test configuration. Run scripts/run_tests to launch "
            << "Docker test databases automatically, or set "
            << "DEARSQL_TEST_PG_HOST / DEARSQL_TEST_PG_DB / DEARSQL_TEST_PG_USER "
            << "(optional PORT/PASSWORD/NAME) before running the tests.";

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::POSTGRESQL;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.database;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = false;

        database = std::make_shared<PostgresDatabase>(connInfo);
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
        ASSERT_TRUE(connected) << "PostgreSQL connection failed: " << lastError;
        tableName = TestHelpers::makeUniqueIdentifier("dearsql_pg_test_");
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
            database->executeQuery(std::format(R"(DROP TABLE IF EXISTS "{}")", tableName));
        }
    }

    PostgresConfig config;
    std::shared_ptr<PostgresDatabase> database;
    std::string tableName;
};

TEST_F(PostgresDatabaseIntegrationTest, ExecuteQueryStructuredReadsInsertedRows) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format(
        R"(CREATE TABLE "{}" (id SERIAL PRIMARY KEY, value TEXT NOT NULL))", tableName));
    auto createSuccess = !r1.empty() && r1[0].success;
    auto createError = r1.empty() ? std::string("No result") : r1[0].errorMessage;
    ASSERT_TRUE(createSuccess) << createError;

    auto r2 = database->executeQuery(
        std::format(R"(INSERT INTO "{}"(value) VALUES ('alpha'), ('beta'), ('gamma'))", tableName));
    auto insertSuccess = !r2.empty() && r2[0].success;
    auto insertError = r2.empty() ? std::string("No result") : r2[0].errorMessage;
    ASSERT_TRUE(insertSuccess) << insertError;

    auto results =
        database->executeQuery(std::format(R"(SELECT value FROM "{}" ORDER BY id)", tableName));
    ASSERT_FALSE(results.empty());
    auto& result = results[0];
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(result.columnNames.size(), 1u);
    EXPECT_EQ(result.columnNames[0], "value");

    // Verify we got data back
    EXPECT_FALSE(result.tableData.empty());
}

TEST_F(PostgresDatabaseIntegrationTest, DropCurrentlyConnectedDatabaseSwitchesToPostgresDatabase) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_pg_drop_active_");

    auto [created, createErr] = database->createDatabase(tempDb);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("permission denied") != std::string::npos ||
            err.find("must be superuser") != std::string::npos ||
            err.find("not enough privileges") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required for this test. Error: "
                         << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    DatabaseConnectionInfo tempConnInfo = database->getConnectionInfo();
    tempConnInfo.database = tempDb;
    auto activeDb = std::make_shared<PostgresDatabase>(tempConnInfo);

    const auto [connected, connectErr] = activeDb->connect();
    ASSERT_TRUE(connected) << connectErr;

    const auto [dropped, dropErr] = activeDb->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
    EXPECT_EQ(activeDb->getConnectionInfo().database, "postgres");

    auto verifyResult = database->executeQuery(
        std::format("SELECT datname FROM pg_database WHERE datname = '{}'", tempDb));
    ASSERT_FALSE(verifyResult.empty());
    ASSERT_TRUE(verifyResult[0].success) << verifyResult[0].errorMessage;
    EXPECT_TRUE(verifyResult[0].tableData.empty());
}

TEST_F(PostgresDatabaseIntegrationTest, CreateDatabaseWithOptionsCreatesDatabaseAndComment) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_pg_create_opts_");

    CreateDatabaseOptions options;
    options.name = tempDb;
    options.owner = "";
    options.templateDb = "template1";
    options.encoding = "UTF8";
    options.tablespace = "pg_default";
    options.comment = "dearsql option test's comment";

    auto [created, createErr] = database->createDatabaseWithOptions(options);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("permission denied") != std::string::npos ||
            err.find("must be superuser") != std::string::npos ||
            err.find("not enough privileges") != std::string::npos ||
            err.find("must be member of role") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required for this test. Error: "
                         << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    auto dbResult = database->executeQuery(
        std::format("SELECT datname, pg_catalog.pg_encoding_to_char(encoding) "
                    "FROM pg_database WHERE datname = '{}'",
                    tempDb));
    ASSERT_FALSE(dbResult.empty());
    ASSERT_TRUE(dbResult[0].success) << dbResult[0].errorMessage;
    ASSERT_EQ(dbResult[0].tableData.size(), 1u);
    EXPECT_EQ(dbResult[0].tableData[0][0], tempDb);
    EXPECT_EQ(dbResult[0].tableData[0][1], "UTF8");

    auto commentResult = database->executeQuery(std::format(
        "SELECT shobj_description(oid, 'pg_database') FROM pg_database WHERE datname = '{}'",
        tempDb));
    ASSERT_FALSE(commentResult.empty());
    ASSERT_TRUE(commentResult[0].success) << commentResult[0].errorMessage;
    ASSERT_EQ(commentResult[0].tableData.size(), 1u);
    EXPECT_EQ(commentResult[0].tableData[0][0], options.comment);

    const auto [dropped, dropErr] = database->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
}
