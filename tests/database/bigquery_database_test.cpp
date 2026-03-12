#include "database/bigquery.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct BigQueryConfig {
        std::string name = "BigQueryIntegration";
        std::string host;
        int port = 9050;
        std::string project;
        std::string dataset;
    };

    std::optional<BigQueryConfig> loadBigQueryConfigFromEnv() {
        BigQueryConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_BQ_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_BQ_PORT");
        const char* projectEnv = std::getenv("DEARSQL_TEST_BQ_PROJECT");
        const char* datasetEnv = std::getenv("DEARSQL_TEST_BQ_DATASET");
        const char* nameEnv = std::getenv("DEARSQL_TEST_BQ_NAME");

        if (!hostEnv || !projectEnv || !datasetEnv) {
            return std::nullopt;
        }

        cfg.host = hostEnv;
        cfg.project = projectEnv;
        cfg.dataset = datasetEnv;
        if (nameEnv && *nameEnv != '\0') {
            cfg.name = nameEnv;
        }
        if (portEnv && *portEnv != '\0') {
            cfg.port = std::stoi(portEnv);
        }

        return cfg;
    }
} // namespace

class BigQueryDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadBigQueryConfigFromEnv();
        if (!cfgOpt.has_value()) {
            GTEST_SKIP() << "Missing BigQuery test configuration. Set DEARSQL_TEST_BQ_HOST / "
                         << "DEARSQL_TEST_BQ_PROJECT / DEARSQL_TEST_BQ_DATASET.";
        }

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::BIGQUERY;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.project; // project ID stored in database field

        database = std::make_shared<BigQueryDatabase>(connInfo);
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
        ASSERT_TRUE(connected) << "BigQuery connection failed: " << lastError;
        tableName = TestHelpers::makeUniqueIdentifier("dearsql_bq_test_");
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
            database->executeQuery(std::format("DROP TABLE IF EXISTS `{}.{}.{}`", config.project,
                                               config.dataset, tableName));
        }
    }

    BigQueryConfig config;
    std::shared_ptr<BigQueryDatabase> database;
    std::string tableName;
};

TEST_F(BigQueryDatabaseIntegrationTest, ExecuteQueryReadsInsertedRows) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format("CREATE TABLE `{}.{}.{}` (id INT64, value STRING)",
                                                 config.project, config.dataset, tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(std::format(
        "INSERT INTO `{}.{}.{}` (id, value) VALUES (1, 'alpha'), (2, 'beta'), (3, 'gamma')",
        config.project, config.dataset, tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto result = database->executeQuery(std::format("SELECT value FROM `{}.{}.{}` ORDER BY id",
                                                     config.project, config.dataset, tableName));
    ASSERT_FALSE(result.empty());
    auto& stmt = result[0];
    ASSERT_TRUE(stmt.success) << stmt.errorMessage;

    ASSERT_EQ(stmt.columnNames.size(), 1u);
    EXPECT_EQ(stmt.columnNames[0], "value");

    ASSERT_EQ(stmt.tableData.size(), 3u);
    EXPECT_EQ(stmt.tableData[0][0], "alpha");
    EXPECT_EQ(stmt.tableData[1][0], "beta");
    EXPECT_EQ(stmt.tableData[2][0], "gamma");
}

TEST_F(BigQueryDatabaseIntegrationTest, ListDatasetsFindsTestDataset) {
    ASSERT_NE(database, nullptr);

    database->checkDatabasesStatusAsync();
    // wait for datasets to load
    for (int i = 0; i < 30 && !database->areDatabasesLoaded(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        database->checkDatabasesStatusAsync();
    }
    ASSERT_TRUE(database->areDatabasesLoaded());

    const auto& dsMap = database->getDatabaseDataMap();
    EXPECT_TRUE(dsMap.contains(config.dataset))
        << "Expected dataset '" << config.dataset << "' not found";
}

TEST_F(BigQueryDatabaseIntegrationTest, CreateAndDropDataset) {
    ASSERT_NE(database, nullptr);

    const std::string tempDataset = TestHelpers::makeUniqueIdentifier("dearsql_bq_ds_");

    auto [created, createErr] = database->createDatabase(tempDataset);
    ASSERT_TRUE(created) << createErr;

    auto [dropped, dropErr] = database->dropDatabase(tempDataset);
    EXPECT_TRUE(dropped) << dropErr;
}

// ========== Dataset Node Tests ==========

class BigQueryDatasetNodeTest : public BigQueryDatabaseIntegrationTest {
protected:
    void SetUp() override {
        BigQueryDatabaseIntegrationTest::SetUp();
        if (testing::Test::HasFatalFailure() || testing::Test::IsSkipped())
            return;

        dsNode = database->getDatabaseData(config.dataset);
        ASSERT_NE(dsNode, nullptr);
    }

    void TearDown() override {
        // wait for any pending async ops before teardown
        if (dsNode) {
            for (int i = 0; i < 10; ++i) {
                dsNode->checkLoadingStatus();
                if (!dsNode->isLoadingTables() && !dsNode->isLoadingViews())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        BigQueryDatabaseIntegrationTest::TearDown();
    }

    BigQueryDatasetNode* dsNode = nullptr;
};

TEST_F(BigQueryDatasetNodeTest, GetTableDataReturnsRows) {
    ASSERT_NE(dsNode, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format("CREATE TABLE `{}.{}.{}` (id INT64, name STRING)",
                                                 config.project, config.dataset, tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(
        std::format("INSERT INTO `{}.{}.{}` (id, name) VALUES (1, 'foo'), (2, 'bar')",
                    config.project, config.dataset, tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto data = dsNode->getTableData(tableName, 100, 0);
    ASSERT_EQ(data.size(), 2u);
}

TEST_F(BigQueryDatasetNodeTest, GetColumnNamesReturnsSchema) {
    ASSERT_NE(dsNode, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r = database->executeQuery(
        std::format("CREATE TABLE `{}.{}.{}` (id INT64, value STRING, active BOOL)", config.project,
                    config.dataset, tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto cols = dsNode->getColumnNames(tableName);
    ASSERT_EQ(cols.size(), 3u);
    EXPECT_EQ(cols[0], "id");
    EXPECT_EQ(cols[1], "value");
    EXPECT_EQ(cols[2], "active");
}

TEST_F(BigQueryDatasetNodeTest, GetRowCountReturnsCorrectCount) {
    ASSERT_NE(dsNode, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format("CREATE TABLE `{}.{}.{}` (id INT64)",
                                                 config.project, config.dataset, tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 =
        database->executeQuery(std::format("INSERT INTO `{}.{}.{}` (id) VALUES (1), (2), (3), (4)",
                                           config.project, config.dataset, tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    int count = dsNode->getRowCount(tableName);
    EXPECT_EQ(count, 4);
}

TEST_F(BigQueryDatasetNodeTest, DropTableRemovesTable) {
    ASSERT_NE(dsNode, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r = database->executeQuery(std::format("CREATE TABLE `{}.{}.{}` (id INT64)",
                                                config.project, config.dataset, tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = dsNode->dropTable(tableName);
    ASSERT_TRUE(ok) << err;

    // verify table is gone
    auto check = database->executeQuery(
        std::format("SELECT * FROM `{}.{}.{}` LIMIT 1", config.project, config.dataset, tableName));
    EXPECT_FALSE(check.success());

    // clear tableName so TearDown doesn't try to drop it again
    tableName.clear();
}

TEST_F(BigQueryDatasetNodeTest, AsyncTablesLoadFindsCreatedTable) {
    ASSERT_NE(dsNode, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r = database->executeQuery(std::format("CREATE TABLE `{}.{}.{}` (id INT64, val STRING)",
                                                config.project, config.dataset, tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    dsNode->startTablesLoadAsync(true);

    // wait for async load
    for (int i = 0; i < 50; ++i) {
        dsNode->checkTablesStatusAsync();
        if (dsNode->isTablesLoaded())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ASSERT_TRUE(dsNode->isTablesLoaded());

    auto& tables = dsNode->getTables();
    auto it = std::ranges::find_if(tables, [this](const Table& t) { return t.name == tableName; });
    ASSERT_NE(it, tables.end()) << "Table " << tableName << " not found in async load";
    EXPECT_FALSE(it->columns.empty()) << "Expected columns to be loaded";
}
