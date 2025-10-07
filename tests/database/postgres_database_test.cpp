#include "database/postgresql.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <ranges>
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
            try {
                cfg.port = std::stoi(portEnv);
            } catch (const std::exception&) {
                // Invalid port supplied, ignore and keep default
            }
        }
        return cfg;
    }
} // namespace

class PostgresDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadPostgresConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing PostgreSQL test configuration. Run scripts/run_tests.sh to launch "
            << "Docker test databases automatically, or set "
            << "DEARSQL_TEST_PG_HOST / DEARSQL_TEST_PG_DB / DEARSQL_TEST_PG_USER "
            << "(optional PORT/PASSWORD/NAME) before running the tests.";

        config = *cfgOpt;
        database = std::make_shared<PostgresDatabase>(config.name, config.host, config.port,
                                                      config.database, config.user, config.password,
                                                      false);
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

    auto result = database->executeQuery(std::format(
        R"(CREATE TABLE "{}" (id SERIAL PRIMARY KEY, value TEXT NOT NULL))", tableName));
    ASSERT_TRUE(result.find("Error") == std::string::npos) << result;

    result = database->executeQuery(
        std::format(R"(INSERT INTO "{}"(value) VALUES ('alpha'), ('beta'), ('gamma'))", tableName));
    ASSERT_TRUE(result.find("Error") == std::string::npos) << result;

    const auto [columns, rows] = database->executeQueryStructured(
        std::format(R"(SELECT value FROM "{}" ORDER BY id)", tableName));
    ASSERT_EQ(columns.size(), 1u);
    EXPECT_EQ(columns[0], "value");

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0], "alpha");
    EXPECT_EQ(rows[1][0], "beta");
    EXPECT_EQ(rows[2][0], "gamma");

    EXPECT_EQ(database->getRowCount(tableName), 3);
    const auto columnNames = database->getColumnNames(tableName);
    EXPECT_TRUE(std::ranges::find(columnNames, "value") != columnNames.end());
}
