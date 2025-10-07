#include "database/sqlite.hpp"

#include <gtest/gtest.h>

class SQLiteDatabaseFixture : public ::testing::Test {
protected:
    SQLiteDatabaseFixture() : database_("TestDB", ":memory:") {}

    void SetUp() override {
        const auto [success, error] = database_.connect();
        ASSERT_TRUE(success) << error;
    }

    void TearDown() override {
        database_.disconnect();
    }

    SQLiteDatabase database_;
};

TEST_F(SQLiteDatabaseFixture, ConnectsToInMemoryDatabase) {
    EXPECT_TRUE(database_.isConnected());
    EXPECT_FALSE(database_.hasAttemptedConnection());
    EXPECT_TRUE(database_.getTables().empty());
}

TEST_F(SQLiteDatabaseFixture, RefreshTablesDetectsCreatedTable) {
    const std::string createTableSql =
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)";
    const std::string createTableResult = database_.executeQuery(createTableSql);
    ASSERT_TRUE(createTableResult.find("Error") == std::string::npos) << createTableResult;

    database_.refreshTables();
    const auto& tables = database_.getTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables.front().name, "users");
    ASSERT_GE(tables.front().columns.size(), 2u);
    EXPECT_EQ(tables.front().columns[0].name, "id");
    EXPECT_TRUE(tables.front().columns[0].isPrimaryKey);
    EXPECT_EQ(tables.front().columns[1].name, "name");
    EXPECT_TRUE(tables.front().columns[1].isNotNull);
}

TEST_F(SQLiteDatabaseFixture, RetrievesInsertedTableData) {
    const std::string createResult = database_.executeQuery(
        "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT)");
    ASSERT_TRUE(createResult.find("Error") == std::string::npos) << createResult;

    ASSERT_TRUE(
        database_.executeQuery("INSERT INTO messages(body) VALUES ('Hello'), ('World'), ('Test');")
            .find("Error") == std::string::npos);

    const auto rows = database_.getTableData("messages", 10, 0);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][1], "Hello");
    EXPECT_EQ(rows[1][1], "World");
    EXPECT_EQ(rows[2][1], "Test");

    const auto columnNames = database_.getColumnNames("messages");
    ASSERT_EQ(columnNames.size(), 2u);
    EXPECT_EQ(columnNames[0], "id");
    EXPECT_EQ(columnNames[1], "body");

    EXPECT_EQ(database_.getRowCount("messages"), 3);
}
