#include "database/duckdb.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

class DuckDBDatabaseFixture : public ::testing::Test {
protected:
    void SetUp() override {
        DatabaseConnectionInfo connInfo;
        connInfo.name = "TestDB";
        connInfo.type = DatabaseType::DUCKDB;
        connInfo.path = ":memory:";

        database_ = std::make_unique<DuckDBDatabase>(connInfo);

        const auto [success, error] = database_->connect();
        ASSERT_TRUE(success) << error;
    }

    void TearDown() override {
        if (database_) {
            database_->disconnect();
        }
    }

    void waitForTables() {
        for (int i = 0; i < 50 && database_->isLoadingTables(); ++i) {
            database_->checkLoadingStatus();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        database_->checkLoadingStatus();
    }

    std::unique_ptr<DuckDBDatabase> database_;
};

TEST_F(DuckDBDatabaseFixture, ConnectsToInMemoryDatabase) {
    EXPECT_TRUE(database_->isConnected());
    EXPECT_TRUE(database_->getTables().empty());
}

TEST_F(DuckDBDatabaseFixture, RefreshTablesDetectsCreatedTable) {
    auto r = database_->executeQuery(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL)");
    ASSERT_TRUE(r.success()) << r.errorMessage();

    database_->startTablesLoadAsync(true);
    waitForTables();

    const auto& tables = database_->getTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables.front().name, "users");
    ASSERT_GE(tables.front().columns.size(), 2u);
    EXPECT_EQ(tables.front().columns[0].name, "id");
    EXPECT_TRUE(tables.front().columns[0].isPrimaryKey);
    EXPECT_EQ(tables.front().columns[1].name, "name");
    EXPECT_TRUE(tables.front().columns[1].isNotNull);
}

TEST_F(DuckDBDatabaseFixture, RetrievesInsertedTableData) {
    ASSERT_TRUE(database_->executeQuery("CREATE TABLE t (id INTEGER, label VARCHAR)").success());
    ASSERT_TRUE(database_->executeQuery("INSERT INTO t VALUES (1, 'one'), (2, 'two')").success());

    Table t;
    t.name = "t";
    auto data = database_->getTableData(t, 100, 0, "");
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0][1], "one");
    EXPECT_EQ(database_->getRowCount(t), 2);

    auto cols = database_->getColumnNames(t);
    ASSERT_EQ(cols.size(), 2u);
    EXPECT_EQ(cols[0], "id");
    EXPECT_EQ(cols[1], "label");
}

TEST_F(DuckDBDatabaseFixture, ExecuteMultipleStatementsReturnsPerStatementResults) {
    auto r = database_->executeQuery("CREATE TABLE m (id INTEGER); "
                                     "INSERT INTO m VALUES (1); "
                                     "SELECT * FROM m");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_EQ(r.statements.size(), 3u);
    EXPECT_EQ(r.statements[1].affectedRows, 1);
    ASSERT_EQ(r.statements[2].tableData.size(), 1u);
    EXPECT_EQ(r.statements[2].tableData[0][0], "1");
}

TEST_F(DuckDBDatabaseFixture, ReportsQueryErrors) {
    auto r = database_->executeQuery("SELECT * FROM missing_table");
    EXPECT_FALSE(r.success());
    EXPECT_FALSE(r.errorMessage().empty());
}

TEST_F(DuckDBDatabaseFixture, IDatabaseNodeInterface) {
    IDatabaseNode* node = database_.get();
    EXPECT_EQ(node->getDatabaseType(), DatabaseType::DUCKDB);
    EXPECT_EQ(node->ownerDatabase(), database_.get());
}

TEST_F(DuckDBDatabaseFixture, LoadsSequences) {
    ASSERT_TRUE(database_->executeQuery("CREATE SEQUENCE serial START 1").success());
    auto sequences = database_->getSequencesAsync();
    ASSERT_EQ(sequences.size(), 1u);
    EXPECT_EQ(sequences.front(), "serial");
}

TEST_F(DuckDBDatabaseFixture, RenameTableRenamesSuccessfully) {
    ASSERT_TRUE(database_->executeQuery("CREATE TABLE old_name (id INTEGER)").success());

    auto [success, error] = database_->renameTable("old_name", "new_name");
    ASSERT_TRUE(success) << error;
    waitForTables();

    const auto& tables = database_->getTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables.front().name, "new_name");
}

TEST_F(DuckDBDatabaseFixture, DropTableRemovesTable) {
    ASSERT_TRUE(database_->executeQuery("CREATE TABLE doomed (id INTEGER)").success());

    auto [success, error] = database_->dropTable("doomed");
    ASSERT_TRUE(success) << error;
    waitForTables();

    EXPECT_TRUE(database_->getTables().empty());
}

TEST_F(DuckDBDatabaseFixture, DropColumnRemovesColumn) {
    ASSERT_TRUE(database_->executeQuery("CREATE TABLE t (id INTEGER, extra VARCHAR)").success());

    auto [success, error] = database_->dropColumn("t", "extra");
    ASSERT_TRUE(success) << error;

    Table t;
    t.name = "t";
    auto cols = database_->getColumnNames(t);
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0], "id");
}

TEST(DuckDBCsvTest, OpensCsvFileAsQueryableTable) {
    const auto csvPath = std::filesystem::temp_directory_path() / "dearsql_duckdb_test.csv";
    {
        std::ofstream out(csvPath);
        out << "id,name\n1,alice\n2,bob\n";
    }

    DatabaseConnectionInfo connInfo;
    connInfo.name = "dearsql_duckdb_test.csv";
    connInfo.type = DatabaseType::DUCKDB;
    connInfo.path = csvPath.string();

    DuckDBDatabase db(connInfo);
    auto [success, error] = db.connect();
    ASSERT_TRUE(success) << error;

    auto tables = db.getTablesAsync();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables.front().name, "dearsql_duckdb_test");

    auto r = db.executeQuery("SELECT name FROM \"dearsql_duckdb_test\" WHERE id = 2");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_EQ(r.statements[0].tableData.size(), 1u);
    EXPECT_EQ(r.statements[0].tableData[0][0], "bob");

    db.disconnect();
    std::filesystem::remove(csvPath);
}

TEST(DuckDBCsvTest, IsCsvPathMatchesCaseInsensitively) {
    EXPECT_TRUE(DuckDBDatabase::isCsvPath("/tmp/data.csv"));
    EXPECT_TRUE(DuckDBDatabase::isCsvPath("C:\\data\\Report.CSV"));
    EXPECT_FALSE(DuckDBDatabase::isCsvPath("/tmp/data.duckdb"));
    EXPECT_FALSE(DuckDBDatabase::isCsvPath(":memory:"));
}

TEST_F(DuckDBDatabaseFixture, NullValuesUseSentinel) {
    ASSERT_TRUE(database_->executeQuery("CREATE TABLE n (v VARCHAR)").success());
    ASSERT_TRUE(database_->executeQuery("INSERT INTO n VALUES (NULL)").success());

    Table t;
    t.name = "n";
    auto data = database_->getTableData(t, 10, 0, "");
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0][0], NULL_SENTINEL);
}
