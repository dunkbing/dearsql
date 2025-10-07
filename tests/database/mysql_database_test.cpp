#include "database/mysql.hpp"
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
            try {
                cfg.port = std::stoi(portEnv);
            } catch (const std::exception&) {
                // keep default
            }
        }

        return cfg;
    }
} // namespace

class MySQLDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadMySQLConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing MySQL test configuration. Run scripts/run_tests.sh to launch "
            << "Docker test databases automatically, or set DEARSQL_TEST_MYSQL_HOST / "
            << "DEARSQL_TEST_MYSQL_DB / DEARSQL_TEST_MYSQL_USER (optional PORT/PASSWORD/NAME).";

        config = *cfgOpt;
        database =
            std::make_shared<MySQLDatabase>(config.name, config.host, config.port, config.database,
                                            config.user, config.password, config.showAllDatabases);
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

    auto result = database->executeQuery(std::format(
        "CREATE TABLE `{}` (id INT PRIMARY KEY AUTO_INCREMENT, value TEXT NOT NULL)", tableName));
    ASSERT_TRUE(result.find("Error") == std::string::npos) << result;

    result = database->executeQuery(
        std::format("INSERT INTO `{}`(value) VALUES ('delta'), ('epsilon'), ('zeta')", tableName));
    ASSERT_TRUE(result.find("Error") == std::string::npos) << result;

    const auto [columns, rows] = database->executeQueryStructured(
        std::format("SELECT value FROM `{}` ORDER BY id", tableName));

    ASSERT_EQ(columns.size(), 1u);
    EXPECT_EQ(columns[0], "value");

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0], "delta");
    EXPECT_EQ(rows[1][0], "epsilon");
    EXPECT_EQ(rows[2][0], "zeta");

    EXPECT_EQ(database->getRowCount(tableName), 3);
    const auto columnNames = database->getColumnNames(tableName);
    EXPECT_TRUE(std::ranges::find(columnNames, "value") != columnNames.end());
}
