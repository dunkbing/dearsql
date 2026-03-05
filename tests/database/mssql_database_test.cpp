#include "database/mssql.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct MSSQLConfig {
        std::string name = "MSSQLIntegration";
        std::string host;
        int port = 1433;
        std::string database;
        std::string user;
        std::string password;
        bool showAllDatabases = false;
    };

    std::optional<MSSQLConfig> loadMSSQLConfigFromEnv() {
        MSSQLConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_MSSQL_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_MSSQL_PORT");
        const char* databaseEnv = std::getenv("DEARSQL_TEST_MSSQL_DB");
        const char* userEnv = std::getenv("DEARSQL_TEST_MSSQL_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_MSSQL_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_MSSQL_NAME");

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

class MSSQLDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadMSSQLConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing MSSQL test configuration. Run scripts/run_tests to launch "
            << "Docker test databases automatically, or set DEARSQL_TEST_MSSQL_HOST / "
            << "DEARSQL_TEST_MSSQL_DB / DEARSQL_TEST_MSSQL_USER (optional PORT/PASSWORD/NAME).";

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::MSSQL;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.database;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = config.showAllDatabases;

        database = std::make_shared<MSSQLDatabase>(connInfo);
        bool connected = false;
        std::string lastError;
        for (int attempt = 0; attempt < 30; ++attempt) {
            try {
                const auto [success, error] = database->connect();
                if (success) {
                    connected = true;
                    break;
                }
                lastError = error;
            } catch (const std::exception& e) {
                lastError = e.what();
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ASSERT_TRUE(connected) << "MSSQL connection failed: " << lastError;
        tableName = TestHelpers::makeUniqueIdentifier("dearsql_mssql_test_");
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
            database->executeQuery(std::format("DROP TABLE IF EXISTS [{}]", tableName));
        }
    }

    MSSQLConfig config;
    std::shared_ptr<MSSQLDatabase> database;
    std::string tableName;
};

TEST_F(MSSQLDatabaseIntegrationTest, ExecuteQueryStructuredReadsInsertedRows) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format(
        "CREATE TABLE [{}] (id INT IDENTITY(1,1) PRIMARY KEY, value NVARCHAR(255) NOT NULL)",
        tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(
        std::format("INSERT INTO [{}](value) VALUES ('delta'), ('epsilon'), ('zeta')", tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto result =
        database->executeQuery(std::format("SELECT value FROM [{}] ORDER BY id", tableName));
    ASSERT_FALSE(result.empty());
    auto& stmt = result[0];
    ASSERT_TRUE(stmt.success) << stmt.errorMessage;

    ASSERT_EQ(stmt.columnNames.size(), 1u);
    EXPECT_EQ(stmt.columnNames[0], "value");

    ASSERT_EQ(stmt.tableData.size(), 3u);
    EXPECT_EQ(stmt.tableData[0][0], "delta");
    EXPECT_EQ(stmt.tableData[1][0], "epsilon");
    EXPECT_EQ(stmt.tableData[2][0], "zeta");
}

TEST_F(MSSQLDatabaseIntegrationTest, DropCurrentlyConnectedDatabaseSwitchesToMasterDatabase) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_mssql_drop_active_");

    auto [created, createErr] = database->createDatabase(tempDb);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("permission") != std::string::npos ||
            err.find("denied") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required. Error: " << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    DatabaseConnectionInfo tempConnInfo = database->getConnectionInfo();
    tempConnInfo.database = tempDb;
    auto activeDb = std::make_shared<MSSQLDatabase>(tempConnInfo);

    const auto [connected, connectErr] = activeDb->connect();
    ASSERT_TRUE(connected) << connectErr;

    const auto [dropped, dropErr] = activeDb->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
    EXPECT_EQ(activeDb->getConnectionInfo().database, "master");

    auto verifyResult = database->executeQuery(
        std::format("SELECT name FROM sys.databases WHERE name = '{}'", tempDb));
    ASSERT_TRUE(verifyResult.success()) << verifyResult.errorMessage();
    ASSERT_FALSE(verifyResult.empty());
    EXPECT_TRUE(verifyResult[0].tableData.empty());
}

// ========== Database Node DDL Tests ==========

class MSSQLDatabaseNodeDDLTest : public MSSQLDatabaseIntegrationTest {
protected:
    void SetUp() override {
        MSSQLDatabaseIntegrationTest::SetUp();
        if (testing::Test::HasFatalFailure())
            return;

        dbNode = database->getDatabaseData(config.database);
        ASSERT_NE(dbNode, nullptr);
    }

    void TearDown() override {
        if (database && !renamedTableName.empty()) {
            database->executeQuery(std::format("DROP TABLE IF EXISTS [{}]", renamedTableName));
        }
        MSSQLDatabaseIntegrationTest::TearDown();
    }

    MSSQLDatabaseNode* dbNode = nullptr;
    std::string renamedTableName;
};

TEST_F(MSSQLDatabaseNodeDDLTest, RenameTableRenamesSuccessfully) {
    auto r = database->executeQuery(
        std::format("CREATE TABLE [{}] (id INT PRIMARY KEY, val NVARCHAR(255))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    renamedTableName = tableName + "_renamed";
    auto [ok, err] = dbNode->renameTable(tableName, renamedTableName);
    ASSERT_TRUE(ok) << err;

    auto check =
        database->executeQuery(std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
                                           "WHERE TABLE_CATALOG='{}' AND TABLE_NAME='{}'",
                                           config.database, renamedTableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData.size(), 1u);
}

TEST_F(MSSQLDatabaseNodeDDLTest, DropTableRemovesTable) {
    auto r =
        database->executeQuery(std::format("CREATE TABLE [{}] (id INT PRIMARY KEY)", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = dbNode->dropTable(tableName);
    ASSERT_TRUE(ok) << err;

    auto check =
        database->executeQuery(std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
                                           "WHERE TABLE_CATALOG='{}' AND TABLE_NAME='{}'",
                                           config.database, tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_TRUE(check[0].tableData.empty());
}

TEST_F(MSSQLDatabaseNodeDDLTest, DropColumnRemovesColumn) {
    auto r = database->executeQuery(std::format(
        "CREATE TABLE [{}] (id INT PRIMARY KEY, keep_me NVARCHAR(255), drop_me NVARCHAR(255))",
        tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = dbNode->dropColumn(tableName, "drop_me");
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
                    "WHERE TABLE_CATALOG='{}' AND TABLE_NAME='{}' ORDER BY ORDINAL_POSITION",
                    config.database, tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    ASSERT_EQ(check[0].tableData.size(), 2u);
    EXPECT_EQ(check[0].tableData[0][0], "id");
    EXPECT_EQ(check[0].tableData[1][0], "keep_me");
}
