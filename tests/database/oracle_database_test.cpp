#include "database/oracle.hpp"
#include "database/oracle/oracle_client_installer.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct OracleConfig {
        std::string name = "OracleIntegration";
        std::string host;
        int port = 1521;
        std::string database; // service name
        std::string user;
        std::string password;
        bool showAllDatabases = false;
    };

    std::optional<OracleConfig> loadOracleConfigFromEnv() {
        OracleConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_ORACLE_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_ORACLE_PORT");
        const char* databaseEnv = std::getenv("DEARSQL_TEST_ORACLE_DB");
        const char* userEnv = std::getenv("DEARSQL_TEST_ORACLE_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_ORACLE_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_ORACLE_NAME");

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

    std::string upperCase(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    }

    bool installerDownloadTestsEnabled() {
        const char* enabledEnv = std::getenv("DEARSQL_TEST_ORACLE_INSTALLER_DOWNLOAD");
        return enabledEnv && std::string(enabledEnv) == "1";
    }
} // namespace

// ========== 1. Oracle Client Installer Tests (run first, no DB needed) ==========

class OracleClientInstallerTest : public ::testing::Test {};

TEST_F(OracleClientInstallerTest, GetInstallDirReturnsNonEmptyPath) {
    auto dir = OracleClientInstaller::getInstallDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_NE(dir.find("oracle-client"), std::string::npos);
}

TEST_F(OracleClientInstallerTest, IsInstalledReturnsBool) {
    [[maybe_unused]] bool installed = OracleClientInstaller::isInstalled();
}

TEST_F(OracleClientInstallerTest, InstallerStartsAndCanBePolled) {
    OracleClientInstaller installer;
    EXPECT_EQ(installer.getStatus(), OracleClientInstaller::Status::Idle);
    EXPECT_FALSE(installer.isRunning());
    EXPECT_TRUE(installer.getError().empty());
}

TEST_F(OracleClientInstallerTest, NeedsClientInstallReflectsContext) {
    [[maybe_unused]] bool needs = OracleDatabase::needsClientInstall();
}

TEST_F(OracleClientInstallerTest, DownloadAndInstallOracleClient) {
    if (!installerDownloadTestsEnabled()) {
        GTEST_SKIP() << "Set DEARSQL_TEST_ORACLE_INSTALLER_DOWNLOAD=1 to run download-based tests";
    }

    if (OracleClientInstaller::isInstalled()) {
        GTEST_SKIP() << "Oracle Client already installed, skipping download test";
    }

    OracleClientInstaller installer;
    installer.startInstall();
    EXPECT_TRUE(installer.isRunning());

    // wait for install to complete (up to 5 minutes for download)
    for (int i = 0; i < 300 && installer.isRunning(); ++i) {
        installer.checkStatus();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    installer.checkStatus();
    ASSERT_EQ(installer.getStatus(), OracleClientInstaller::Status::Done)
        << "Install failed: " << installer.getError();
    EXPECT_TRUE(OracleClientInstaller::isInstalled());

    // re-initialize ODPI-C context with newly installed client
    OracleDatabase::reinitContext();
    EXPECT_FALSE(OracleDatabase::needsClientInstall())
        << "Context should initialize after client install";
}

// ========== 2. Oracle Database Integration Tests (require client + DB) ==========

class OracleDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // skip immediately if Oracle Client is not available
        if (OracleDatabase::needsClientInstall()) {
            GTEST_SKIP() << "Oracle Instant Client not available, skipping integration test";
        }

        const auto cfgOpt = loadOracleConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing Oracle test configuration. Set DEARSQL_TEST_ORACLE_HOST / "
            << "DEARSQL_TEST_ORACLE_DB / DEARSQL_TEST_ORACLE_USER (optional PORT/PASSWORD/NAME).";

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::ORACLE;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.database;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = config.showAllDatabases;

        database = std::make_shared<OracleDatabase>(connInfo);
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
        ASSERT_TRUE(connected) << "Oracle connection failed: " << lastError;

        tableName = upperCase(TestHelpers::makeUniqueIdentifier("DSQL_ORA_"));
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
            database->executeQuery(
                std::format("BEGIN EXECUTE IMMEDIATE 'DROP TABLE \"{}\" CASCADE CONSTRAINTS'; "
                            "EXCEPTION WHEN OTHERS THEN NULL; END;",
                            tableName));
        }
    }

    OracleConfig config;
    std::shared_ptr<OracleDatabase> database;
    std::string tableName;
};

TEST_F(OracleDatabaseIntegrationTest, ConnectionSucceeds) {
    EXPECT_TRUE(database->isConnected());
}

TEST_F(OracleDatabaseIntegrationTest, ExecuteSimpleQuery) {
    auto result = database->executeQuery("SELECT 1 AS VAL FROM DUAL");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.statements.empty());
    EXPECT_FALSE(result.statements[0].tableData.empty());
}

TEST_F(OracleDatabaseIntegrationTest, ExecuteQueryStructuredReadsInsertedRows) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format(
        "CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY, value VARCHAR2(255) NOT NULL)", tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(std::format("INSERT ALL "
                                                 "INTO \"{}\" (id, value) VALUES (1, 'delta') "
                                                 "INTO \"{}\" (id, value) VALUES (2, 'epsilon') "
                                                 "INTO \"{}\" (id, value) VALUES (3, 'zeta') "
                                                 "SELECT 1 FROM DUAL",
                                                 tableName, tableName, tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto result =
        database->executeQuery(std::format("SELECT value FROM \"{}\" ORDER BY id", tableName));
    ASSERT_FALSE(result.empty());
    auto& stmt = result[0];
    ASSERT_TRUE(stmt.success) << stmt.errorMessage;

    ASSERT_EQ(stmt.columnNames.size(), 1u);
    EXPECT_EQ(stmt.columnNames[0], "VALUE");

    ASSERT_EQ(stmt.tableData.size(), 3u);
    EXPECT_EQ(stmt.tableData[0][0], "delta");
    EXPECT_EQ(stmt.tableData[1][0], "epsilon");
    EXPECT_EQ(stmt.tableData[2][0], "zeta");
}

TEST_F(OracleDatabaseIntegrationTest, CreateAndDropSchema) {
    ASSERT_NE(database, nullptr);

    const std::string tempSchema = upperCase(TestHelpers::makeUniqueIdentifier("DSQL_ORA_SCH_"));

    auto [created, createErr] = database->createDatabase(tempSchema);
    if (!created) {
        if (createErr.find("insufficient privileges") != std::string::npos ||
            createErr.find("ORA-01031") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE USER privilege required. Error: " << createErr;
        }
    }
    ASSERT_TRUE(created) << createErr;

    auto check = database->executeQuery(
        std::format("SELECT USERNAME FROM ALL_USERS WHERE USERNAME = '{}'", tempSchema));
    ASSERT_TRUE(check.success()) << check.errorMessage();
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData.size(), 1u);

    auto [dropped, dropErr] = database->dropDatabase(tempSchema);
    ASSERT_TRUE(dropped) << dropErr;

    auto verify = database->executeQuery(
        std::format("SELECT USERNAME FROM ALL_USERS WHERE USERNAME = '{}'", tempSchema));
    ASSERT_TRUE(verify.success()) << verify.errorMessage();
    ASSERT_FALSE(verify.empty());
    EXPECT_TRUE(verify[0].tableData.empty());
}

// ========== 3. Database Node Tests ==========

class OracleDatabaseNodeTest : public OracleDatabaseIntegrationTest {
protected:
    void SetUp() override {
        OracleDatabaseIntegrationTest::SetUp();
        if (testing::Test::HasFatalFailure() || testing::Test::IsSkipped())
            return;

        schemaName = upperCase(config.user);
        dbNode = database->getDatabaseData(schemaName);
        ASSERT_NE(dbNode, nullptr);
    }

    void TearDown() override {
        if (database && !renamedTableName.empty()) {
            database->executeQuery(
                std::format("BEGIN EXECUTE IMMEDIATE 'DROP TABLE \"{}\" CASCADE CONSTRAINTS'; "
                            "EXCEPTION WHEN OTHERS THEN NULL; END;",
                            renamedTableName));
        }
        OracleDatabaseIntegrationTest::TearDown();
    }

    OracleDatabaseNode* dbNode = nullptr;
    std::string schemaName;
    std::string renamedTableName;
};

TEST_F(OracleDatabaseNodeTest, LoadTablesAsync) {
    dbNode->startTablesLoadAsync(true);

    for (int i = 0; i < 300 && dbNode->isLoadingTables(); ++i) {
        dbNode->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(dbNode->isTablesLoaded());
    EXPECT_TRUE(dbNode->getLastTablesError().empty()) << dbNode->getLastTablesError();
}

TEST_F(OracleDatabaseNodeTest, LoadViewsAsync) {
    dbNode->startViewsLoadAsync(true);

    for (int i = 0; i < 300 && dbNode->isLoadingViews(); ++i) {
        dbNode->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(dbNode->isViewsLoaded());
    EXPECT_TRUE(dbNode->getLastViewsError().empty()) << dbNode->getLastViewsError();
}

TEST_F(OracleDatabaseNodeTest, GetTableDataReturnsPaginatedRows) {
    auto r1 = dbNode->executeQuery(
        std::format("CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY, name VARCHAR2(100))", tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    for (int i = 1; i <= 5; ++i) {
        auto ri = dbNode->executeQuery(
            std::format("INSERT INTO \"{}\" (id, name) VALUES ({}, 'row{}')", tableName, i, i));
        ASSERT_TRUE(ri.success()) << ri.errorMessage();
    }

    const Table table{.name = tableName, .schema = schemaName};
    auto data = dbNode->getTableData(table, 3, 0);
    EXPECT_EQ(data.size(), 3u);

    auto dataOffset = dbNode->getTableData(table, 3, 2);
    EXPECT_EQ(dataOffset.size(), 3u);
}

TEST_F(OracleDatabaseNodeTest, GetColumnNamesReturnsColumns) {
    auto r1 = dbNode->executeQuery(std::format(
        "CREATE TABLE \"{}\" (id NUMBER, name VARCHAR2(100), active NUMBER(1))", tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto columns = dbNode->getColumnNames(Table{.name = tableName, .schema = schemaName});
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_EQ(columns[0], "ID");
    EXPECT_EQ(columns[1], "NAME");
    EXPECT_EQ(columns[2], "ACTIVE");
}

TEST_F(OracleDatabaseNodeTest, GetRowCountReturnsCorrectCount) {
    auto r1 =
        dbNode->executeQuery(std::format("CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY)", tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    const Table table{.name = tableName, .schema = schemaName};
    EXPECT_EQ(dbNode->getRowCount(table), 0);

    for (int i = 1; i <= 3; ++i) {
        dbNode->executeQuery(std::format("INSERT INTO \"{}\" (id) VALUES ({})", tableName, i));
    }

    EXPECT_EQ(dbNode->getRowCount(table), 3);

    EXPECT_EQ(dbNode->getRowCount(table, "id > 1"), 2);
}

TEST_F(OracleDatabaseNodeTest, RenameTableRenamesSuccessfully) {
    auto r = dbNode->executeQuery(
        std::format("CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY, val VARCHAR2(255))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    renamedTableName = tableName + "_REN";
    auto [ok, err] = dbNode->renameTable(tableName, renamedTableName);
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT TABLE_NAME FROM ALL_TABLES WHERE OWNER = '{}' AND TABLE_NAME = '{}'",
                    schemaName, renamedTableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData.size(), 1u);
}

TEST_F(OracleDatabaseNodeTest, DropTableRemovesTable) {
    auto r =
        dbNode->executeQuery(std::format("CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY)", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = dbNode->dropTable(tableName);
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT TABLE_NAME FROM ALL_TABLES WHERE OWNER = '{}' AND TABLE_NAME = '{}'",
                    schemaName, tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_TRUE(check[0].tableData.empty());
}

TEST_F(OracleDatabaseNodeTest, TruncateTableRemovesAllRows) {
    auto r = dbNode->executeQuery(
        std::format("CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY, val VARCHAR2(255))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    for (const auto& [id, value] : {std::pair{1, "a"}, std::pair{2, "b"}, std::pair{3, "c"}}) {
        auto ins = dbNode->executeQuery(
            std::format("INSERT INTO \"{}\" (id, val) VALUES ({}, '{}')", tableName, id, value));
        ASSERT_TRUE(ins.success()) << ins.errorMessage();
    }

    auto pre = dbNode->executeQuery(std::format("SELECT COUNT(*) FROM \"{}\"", tableName));
    ASSERT_TRUE(pre.success());
    ASSERT_FALSE(pre.empty());
    EXPECT_EQ(pre[0].tableData[0][0], "3");

    auto [ok, err] = dbNode->truncateTable(tableName);
    ASSERT_TRUE(ok) << err;

    auto check = dbNode->executeQuery(std::format("SELECT COUNT(*) FROM \"{}\"", tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData[0][0], "0");
}

TEST_F(OracleDatabaseNodeTest, DropColumnRemovesColumn) {
    auto r = dbNode->executeQuery(std::format(
        "CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY, keep_me VARCHAR2(255), drop_me VARCHAR2(255))",
        tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = dbNode->dropColumn(tableName, "DROP_ME");
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT COLUMN_NAME FROM ALL_TAB_COLUMNS "
                    "WHERE OWNER = '{}' AND TABLE_NAME = '{}' ORDER BY COLUMN_ID",
                    schemaName, tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    ASSERT_EQ(check[0].tableData.size(), 2u);
    EXPECT_EQ(check[0].tableData[0][0], "ID");
    EXPECT_EQ(check[0].tableData[1][0], "KEEP_ME");
}

TEST_F(OracleDatabaseNodeTest, TableRefreshAsyncReloadsTable) {
    auto r = dbNode->executeQuery(
        std::format("CREATE TABLE \"{}\" (id NUMBER PRIMARY KEY, name VARCHAR2(100))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    dbNode->startTablesLoadAsync(true);
    for (int i = 0; i < 100 && dbNode->isLoadingTables(); ++i) {
        dbNode->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(dbNode->isTablesLoaded());

    dbNode->startTableRefreshAsync(tableName);
    for (int i = 0; i < 100 && dbNode->isTableRefreshing(tableName); ++i) {
        dbNode->checkTableRefreshStatusAsync(tableName);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_FALSE(dbNode->isTableRefreshing(tableName));
}

TEST_F(OracleDatabaseNodeTest, ExecuteQueryViaDatabaseNode) {
    auto result = dbNode->executeQuery("SELECT SYSDATE FROM DUAL");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.statements.empty());
    EXPECT_FALSE(result.statements[0].tableData.empty());
}
