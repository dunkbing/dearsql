#include "database/duckdb.hpp"
#include <filesystem>
#include <gtest/gtest.h>

class DuckDBDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = "/tmp/dearsql_duckdb_test.duckdb";
        std::filesystem::remove(path);

        conn.type = DatabaseType::DUCKDB;
        conn.name = "duckdb_test";
        conn.path = path;

        db = std::make_unique<DuckDBDatabase>(conn);
        auto [ok, err] = db->connect();
        ASSERT_TRUE(ok) << err;
    }

    void TearDown() override {
        if (db) {
            db->disconnect();
        }
        std::filesystem::remove(path);
    }

    std::string path;
    DatabaseConnectionInfo conn;
    std::unique_ptr<DuckDBDatabase> db;
};

TEST_F(DuckDBDatabaseTest, ExecuteAndLoadTables) {
    auto createResult = db->executeQuery("CREATE TABLE users(id INTEGER, name VARCHAR)");
    ASSERT_TRUE(createResult.success()) << createResult.errorMessage();

    auto insertResult = db->executeQuery("INSERT INTO users VALUES (1, 'alice'), (2, 'bob')");
    ASSERT_TRUE(insertResult.success()) << insertResult.errorMessage();

    auto selectResult = db->executeQuery("SELECT name FROM users ORDER BY id");
    ASSERT_TRUE(selectResult.success()) << selectResult.errorMessage();
    ASSERT_EQ(selectResult[0].tableData.size(), 2);
    EXPECT_EQ(selectResult[0].tableData[0][0], "alice");

    db->startTablesLoadAsync(true);
    while (db->isLoadingTables()) {
        db->checkLoadingStatus();
    }

    ASSERT_TRUE(db->isTablesLoaded());
    ASSERT_EQ(db->getTables().size(), 1);
    EXPECT_EQ(db->getTables()[0].name, "users");
}
