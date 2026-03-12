#include "database/duckdb.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

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

    void waitForTables() {
        for (int i = 0; i < 50 && db->isLoadingTables(); ++i) {
            db->checkLoadingStatus();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void waitForViews() {
        for (int i = 0; i < 50 && db->isLoadingViews(); ++i) {
            db->checkLoadingStatus();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::string path;
    DatabaseConnectionInfo conn;
    std::unique_ptr<DuckDBDatabase> db;
};

TEST_F(DuckDBDatabaseTest, ConnectsSuccessfully) {
    EXPECT_TRUE(db->isConnected());
    EXPECT_EQ(db->getDatabaseType(), DatabaseType::DUCKDB);
}

TEST_F(DuckDBDatabaseTest, IDatabaseNodeInterface) {
    EXPECT_FALSE(db->getName().empty());
    EXPECT_EQ(db->getDatabaseType(), DatabaseType::DUCKDB);
    EXPECT_FALSE(db->getFullPath().empty());
    EXPECT_EQ(db->getFullPath(), path);
}

TEST_F(DuckDBDatabaseTest, ExecuteQueryReturnsData) {
    auto r1 = db->executeQuery("CREATE TABLE users(id INTEGER, name VARCHAR)");
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = db->executeQuery("INSERT INTO users VALUES (1, 'alice'), (2, 'bob')");
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto result = db->executeQuery("SELECT name FROM users ORDER BY id");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_FALSE(result.empty());
    auto& stmt = result[0];
    ASSERT_EQ(stmt.columnNames.size(), 1u);
    EXPECT_EQ(stmt.columnNames[0], "name");
    ASSERT_EQ(stmt.tableData.size(), 2u);
    EXPECT_EQ(stmt.tableData[0][0], "alice");
    EXPECT_EQ(stmt.tableData[1][0], "bob");
}

TEST_F(DuckDBDatabaseTest, ExecuteQueryReportsErrors) {
    auto result = db->executeQuery("SELECT * FROM nonexistent_table");
    EXPECT_FALSE(result.success());
    EXPECT_FALSE(result.errorMessage().empty());
}

TEST_F(DuckDBDatabaseTest, ExecuteQueryRespectsRowLimit) {
    db->executeQuery("CREATE TABLE nums(n INTEGER)");
    db->executeQuery("INSERT INTO nums SELECT * FROM range(100)");

    auto result = db->executeQuery("SELECT * FROM nums", 5);
    ASSERT_TRUE(result.success()) << result.errorMessage();
    EXPECT_EQ(result[0].tableData.size(), 5u);
}

TEST_F(DuckDBDatabaseTest, GetTableDataReturnsRows) {
    db->executeQuery("CREATE TABLE messages(id INTEGER, body VARCHAR)");
    db->executeQuery("INSERT INTO messages VALUES (1, 'Hello'), (2, 'World'), (3, 'Test')");

    auto rows = db->getTableData("messages", 10, 0);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][1], "Hello");
    EXPECT_EQ(rows[1][1], "World");
    EXPECT_EQ(rows[2][1], "Test");
}

TEST_F(DuckDBDatabaseTest, GetTableDataWithOffsetAndLimit) {
    db->executeQuery("CREATE TABLE items(id INTEGER)");
    db->executeQuery("INSERT INTO items SELECT * FROM range(1, 11)");

    auto rows = db->getTableData("items", 3, 2);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0], "3");
    EXPECT_EQ(rows[1][0], "4");
    EXPECT_EQ(rows[2][0], "5");
}

TEST_F(DuckDBDatabaseTest, GetTableDataWithWhereClause) {
    db->executeQuery("CREATE TABLE filtered(id INTEGER, status VARCHAR)");
    db->executeQuery(
        "INSERT INTO filtered VALUES (1,'active'),(2,'inactive'),(3,'active'),(4,'inactive')");

    auto rows = db->getTableData("filtered", 100, 0, "status = 'active'");
    ASSERT_EQ(rows.size(), 2u);
}

TEST_F(DuckDBDatabaseTest, GetTableDataWithOrderBy) {
    db->executeQuery("CREATE TABLE ordered(id INTEGER, name VARCHAR)");
    db->executeQuery("INSERT INTO ordered VALUES (3, 'charlie'), (1, 'alice'), (2, 'bob')");

    auto rows = db->getTableData("ordered", 100, 0, "", "name ASC");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][1], "alice");
    EXPECT_EQ(rows[1][1], "bob");
    EXPECT_EQ(rows[2][1], "charlie");
}

TEST_F(DuckDBDatabaseTest, GetColumnNamesReturnsSchema) {
    db->executeQuery("CREATE TABLE schema_test(id INTEGER, name VARCHAR, active BOOLEAN)");

    auto cols = db->getColumnNames("schema_test");
    ASSERT_EQ(cols.size(), 3u);
    EXPECT_EQ(cols[0], "id");
    EXPECT_EQ(cols[1], "name");
    EXPECT_EQ(cols[2], "active");
}

TEST_F(DuckDBDatabaseTest, GetRowCountReturnsCorrectCount) {
    db->executeQuery("CREATE TABLE counted(id INTEGER)");
    db->executeQuery("INSERT INTO counted SELECT * FROM range(7)");

    EXPECT_EQ(db->getRowCount("counted"), 7);
}

TEST_F(DuckDBDatabaseTest, GetRowCountWithWhereClause) {
    db->executeQuery("CREATE TABLE filtered_count(id INTEGER, flag BOOLEAN)");
    db->executeQuery(
        "INSERT INTO filtered_count VALUES (1,true),(2,false),(3,true),(4,true),(5,false)");

    EXPECT_EQ(db->getRowCount("filtered_count", "flag = true"), 3);
}

TEST_F(DuckDBDatabaseTest, AsyncTablesLoadFindsTables) {
    db->executeQuery("CREATE TABLE t1(id INTEGER)");
    db->executeQuery("CREATE TABLE t2(id INTEGER)");

    db->startTablesLoadAsync(true);
    waitForTables();

    ASSERT_TRUE(db->isTablesLoaded());
    auto& tables = db->getTables();
    ASSERT_EQ(tables.size(), 2u);

    std::vector<std::string> names;
    for (const auto& t : tables)
        names.push_back(t.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "t1");
    EXPECT_EQ(names[1], "t2");
}

TEST_F(DuckDBDatabaseTest, AsyncTablesLoadIncludesColumnMetadata) {
    db->executeQuery("CREATE TABLE meta_test(id INTEGER NOT NULL, name VARCHAR)");

    db->startTablesLoadAsync(true);
    waitForTables();

    ASSERT_TRUE(db->isTablesLoaded());
    ASSERT_EQ(db->getTables().size(), 1u);
    auto& table = db->getTables()[0];
    EXPECT_EQ(table.name, "meta_test");
    ASSERT_GE(table.columns.size(), 2u);
    EXPECT_EQ(table.columns[0].name, "id");
    EXPECT_EQ(table.columns[1].name, "name");
}

TEST_F(DuckDBDatabaseTest, AsyncViewsLoadFindsViews) {
    db->executeQuery("CREATE TABLE base(id INTEGER)");
    db->executeQuery("CREATE VIEW my_view AS SELECT * FROM base");

    db->startViewsLoadAsync(true);
    waitForViews();

    ASSERT_TRUE(db->isViewsLoaded());
    auto& views = db->getViews();
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views[0].name, "my_view");
}

TEST_F(DuckDBDatabaseTest, ForceRefreshReloadsData) {
    db->executeQuery("CREATE TABLE initial(id INTEGER)");
    db->startTablesLoadAsync(true);
    waitForTables();
    ASSERT_EQ(db->getTables().size(), 1u);

    db->executeQuery("CREATE TABLE added(id INTEGER)");
    db->startTablesLoadAsync(true);
    waitForTables();
    EXPECT_EQ(db->getTables().size(), 2u);
}

TEST_F(DuckDBDatabaseTest, TableRefreshUpdatesSpecificTable) {
    db->executeQuery("CREATE TABLE refresh_me(id INTEGER)");
    db->startTablesLoadAsync(true);
    waitForTables();
    ASSERT_EQ(db->getTables().size(), 1u);

    // add a column
    db->executeQuery("ALTER TABLE refresh_me ADD COLUMN name VARCHAR");

    db->startTableRefreshAsync("refresh_me");
    for (int i = 0; i < 50 && db->isTableRefreshing("refresh_me"); ++i) {
        db->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(db->isTableRefreshing("refresh_me"));

    auto& tables = db->getTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0].columns.size(), 2u);
}

TEST_F(DuckDBDatabaseTest, DisconnectAndReconnect) {
    db->executeQuery("CREATE TABLE persist(id INTEGER)");
    db->executeQuery("INSERT INTO persist VALUES (42)");
    db->disconnect();
    EXPECT_FALSE(db->isConnected());

    auto [ok, err] = db->connect();
    ASSERT_TRUE(ok) << err;
    EXPECT_TRUE(db->isConnected());

    auto result = db->executeQuery("SELECT id FROM persist");
    ASSERT_TRUE(result.success()) << result.errorMessage();
    ASSERT_EQ(result[0].tableData.size(), 1u);
    EXPECT_EQ(result[0].tableData[0][0], "42");
}

TEST_F(DuckDBDatabaseTest, NullValuesReturnedAsNULL) {
    db->executeQuery("CREATE TABLE nulls(id INTEGER, val VARCHAR)");
    db->executeQuery("INSERT INTO nulls VALUES (1, NULL)");

    auto rows = db->getTableData("nulls", 10, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][1], "NULL");
}

TEST_F(DuckDBDatabaseTest, CreateTableViaDDLBuilder) {
    Table table;
    table.name = "created_table";
    Column c1;
    c1.name = "id";
    c1.type = "INTEGER";
    c1.isPrimaryKey = true;
    Column c2;
    c2.name = "value";
    c2.type = "VARCHAR";
    table.columns = {c1, c2};

    auto [ok, err] = db->createTable(table);
    ASSERT_TRUE(ok) << err;

    auto cols = db->getColumnNames("created_table");
    ASSERT_EQ(cols.size(), 2u);
    EXPECT_EQ(cols[0], "id");
    EXPECT_EQ(cols[1], "value");
}
