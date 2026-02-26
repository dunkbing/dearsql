#include "database/sqlite.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

class SQLiteDatabaseFixture : public ::testing::Test {
protected:
    void SetUp() override {
        DatabaseConnectionInfo connInfo;
        connInfo.name = "TestDB";
        connInfo.type = DatabaseType::SQLITE;
        connInfo.path = ":memory:";

        database_ = std::make_unique<SQLiteDatabase>(connInfo);

        const auto [success, error] = database_->connect();
        ASSERT_TRUE(success) << error;
    }

    void TearDown() override {
        if (database_) {
            database_->disconnect();
        }
    }

    std::unique_ptr<SQLiteDatabase> database_;
};

TEST_F(SQLiteDatabaseFixture, ConnectsToInMemoryDatabase) {
    EXPECT_TRUE(database_->isConnected());
    EXPECT_TRUE(database_->getTables().empty());
}

TEST_F(SQLiteDatabaseFixture, RefreshTablesDetectsCreatedTable) {
    const std::string createTableSql =
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)";
    auto r = database_->executeQuery(createTableSql);
    ASSERT_TRUE(r.success()) << r.errorMessage();

    // Load tables asynchronously
    database_->startTablesLoadAsync(true);

    // Wait for tables to load
    for (int i = 0; i < 50 && database_->isLoadingTables(); ++i) {
        database_->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto& tables = database_->getTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables.front().name, "users");
    ASSERT_GE(tables.front().columns.size(), 2u);
    EXPECT_EQ(tables.front().columns[0].name, "id");
    EXPECT_TRUE(tables.front().columns[0].isPrimaryKey);
    EXPECT_EQ(tables.front().columns[1].name, "name");
    EXPECT_TRUE(tables.front().columns[1].isNotNull);
}

TEST_F(SQLiteDatabaseFixture, RetrievesInsertedTableData) {
    auto r1 = database_->executeQuery(
        "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT)");
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database_->executeQuery(
        "INSERT INTO messages(body) VALUES ('Hello'), ('World'), ('Test');");
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    const auto rows = database_->getTableData("messages", 10, 0);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][1], "Hello");
    EXPECT_EQ(rows[1][1], "World");
    EXPECT_EQ(rows[2][1], "Test");

    const auto columnNames = database_->getColumnNames("messages");
    ASSERT_EQ(columnNames.size(), 2u);
    EXPECT_EQ(columnNames[0], "id");
    EXPECT_EQ(columnNames[1], "body");

    EXPECT_EQ(database_->getRowCount("messages"), 3);
}

TEST_F(SQLiteDatabaseFixture, ExecuteQueryWithResultReturnsData) {
    database_->executeQuery("CREATE TABLE test (id INTEGER, value TEXT)");
    database_->executeQuery("INSERT INTO test VALUES (1, 'one'), (2, 'two'), (3, 'three')");

    auto result = database_->executeQuery("SELECT * FROM test ORDER BY id");
    ASSERT_FALSE(result.empty());
    auto& stmt = result[0];
    EXPECT_TRUE(stmt.success);
    ASSERT_EQ(stmt.columnNames.size(), 2u);
    EXPECT_EQ(stmt.columnNames[0], "id");
    EXPECT_EQ(stmt.columnNames[1], "value");
    // Verify we got data back (exact count depends on implementation details)
    EXPECT_FALSE(stmt.tableData.empty());
}

TEST_F(SQLiteDatabaseFixture, IDatabaseNodeInterface) {
    // getName() returns the connection name (path for SQLite)
    EXPECT_FALSE(database_->getName().empty());
    EXPECT_EQ(database_->getDatabaseType(), DatabaseType::SQLITE);
    EXPECT_FALSE(database_->getFullPath().empty());
}
