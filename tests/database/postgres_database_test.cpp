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

    auto [createSuccess, createError] = database->executeQuery(std::format(
        R"(CREATE TABLE "{}" (id SERIAL PRIMARY KEY, value TEXT NOT NULL))", tableName));
    ASSERT_TRUE(createSuccess) << createError;

    auto [insertSuccess, insertError] = database->executeQuery(
        std::format(R"(INSERT INTO "{}"(value) VALUES ('alpha'), ('beta'), ('gamma'))", tableName));
    ASSERT_TRUE(insertSuccess) << insertError;

    auto result = database->executeQueryWithResult(
        std::format(R"(SELECT value FROM "{}" ORDER BY id)", tableName));
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_EQ(result.columnNames.size(), 1u);
    EXPECT_EQ(result.columnNames[0], "value");

    // Verify we got data back
    EXPECT_FALSE(result.tableData.empty());
}
