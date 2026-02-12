#include "database/sql_builder.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

class SQLBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        postgresBuilder = createSQLBuilder(DatabaseType::POSTGRESQL);
        mysqlBuilder = createSQLBuilder(DatabaseType::MYSQL);
        sqliteBuilder = createSQLBuilder(DatabaseType::SQLITE);
    }

    std::unique_ptr<ISQLBuilder> postgresBuilder;
    std::unique_ptr<ISQLBuilder> mysqlBuilder;
    std::unique_ptr<ISQLBuilder> sqliteBuilder;
};

// ========== Identifier Quoting Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLQuotesIdentifiersWithDoubleQuotes) {
    EXPECT_EQ(postgresBuilder->quoteIdentifier("users"), "\"users\"");
    EXPECT_EQ(postgresBuilder->quoteIdentifier("user_name"), "\"user_name\"");
}

TEST_F(SQLBuilderTest, MySQLQuotesIdentifiersWithBackticks) {
    EXPECT_EQ(mysqlBuilder->quoteIdentifier("users"), "`users`");
    EXPECT_EQ(mysqlBuilder->quoteIdentifier("user_name"), "`user_name`");
}

TEST_F(SQLBuilderTest, SQLiteQuotesIdentifiersWithDoubleQuotes) {
    EXPECT_EQ(sqliteBuilder->quoteIdentifier("users"), "\"users\"");
    EXPECT_EQ(sqliteBuilder->quoteIdentifier("user_name"), "\"user_name\"");
}

// ========== String Quoting Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLQuotesStrings) {
    EXPECT_EQ(postgresBuilder->quoteString("hello"), "'hello'");
    EXPECT_EQ(postgresBuilder->quoteString("it's"), "'it''s'");
}

TEST_F(SQLBuilderTest, MySQLQuotesStrings) {
    EXPECT_EQ(mysqlBuilder->quoteString("hello"), "'hello'");
    EXPECT_EQ(mysqlBuilder->quoteString("it's"), "'it''s'");
}

TEST_F(SQLBuilderTest, SQLiteQuotesStrings) {
    EXPECT_EQ(sqliteBuilder->quoteString("hello"), "'hello'");
    EXPECT_EQ(sqliteBuilder->quoteString("it's"), "'it''s'");
}

// ========== SELECT Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLSelectAll) {
    std::string sql = postgresBuilder->selectAll("users", 100, 0);
    EXPECT_EQ(sql, "SELECT * FROM \"users\" LIMIT 100 OFFSET 0");
}

TEST_F(SQLBuilderTest, PostgreSQLSelectAllWithOffset) {
    std::string sql = postgresBuilder->selectAll("users", 50, 100);
    EXPECT_EQ(sql, "SELECT * FROM \"users\" LIMIT 50 OFFSET 100");
}

TEST_F(SQLBuilderTest, MySQLSelectAll) {
    std::string sql = mysqlBuilder->selectAll("users", 100, 0);
    EXPECT_EQ(sql, "SELECT * FROM `users` LIMIT 100 OFFSET 0");
}

TEST_F(SQLBuilderTest, SQLiteSelectAll) {
    std::string sql = sqliteBuilder->selectAll("users", 100, 0);
    EXPECT_EQ(sql, "SELECT * FROM \"users\" LIMIT 100 OFFSET 0");
}

TEST_F(SQLBuilderTest, PostgreSQLSelectWithFilter) {
    std::string sql = postgresBuilder->selectWithFilter("users", "age > 18", "name ASC", 50, 0);
    EXPECT_EQ(sql, "SELECT * FROM \"users\" WHERE age > 18 ORDER BY name ASC LIMIT 50 OFFSET 0");
}

TEST_F(SQLBuilderTest, PostgreSQLSelectWithFilterNoOrderBy) {
    std::string sql = postgresBuilder->selectWithFilter("users", "age > 18", "", 50, 0);
    EXPECT_EQ(sql, "SELECT * FROM \"users\" WHERE age > 18 LIMIT 50 OFFSET 0");
}

TEST_F(SQLBuilderTest, PostgreSQLSelectWithFilterNoWhere) {
    std::string sql = postgresBuilder->selectWithFilter("users", "", "name ASC", 50, 0);
    EXPECT_EQ(sql, "SELECT * FROM \"users\" ORDER BY name ASC LIMIT 50 OFFSET 0");
}

TEST_F(SQLBuilderTest, PostgreSQLSelectCount) {
    std::string sql = postgresBuilder->selectCount("users");
    EXPECT_EQ(sql, "SELECT COUNT(*) FROM \"users\"");
}

TEST_F(SQLBuilderTest, PostgreSQLSelectCountWithWhere) {
    std::string sql = postgresBuilder->selectCount("users", "age > 18");
    EXPECT_EQ(sql, "SELECT COUNT(*) FROM \"users\" WHERE age > 18");
}

// ========== UPDATE Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLUpdate) {
    std::vector<std::pair<std::string, std::string>> whereConditions = {{"id", "1"}};
    std::string sql = postgresBuilder->update("users", "name", "Alice", whereConditions);
    EXPECT_EQ(sql, "UPDATE \"users\" SET \"name\" = 'Alice' WHERE \"id\" = '1'");
}

TEST_F(SQLBuilderTest, PostgreSQLUpdateMultipleConditions) {
    std::vector<std::pair<std::string, std::string>> whereConditions = {{"id", "1"},
                                                                        {"status", "active"}};
    std::string sql = postgresBuilder->update("users", "name", "Bob", whereConditions);
    EXPECT_EQ(sql,
              "UPDATE \"users\" SET \"name\" = 'Bob' WHERE \"id\" = '1' AND \"status\" = 'active'");
}

TEST_F(SQLBuilderTest, MySQLUpdate) {
    std::vector<std::pair<std::string, std::string>> whereConditions = {{"id", "1"}};
    std::string sql = mysqlBuilder->update("users", "name", "Alice", whereConditions);
    EXPECT_EQ(sql, "UPDATE `users` SET `name` = 'Alice' WHERE `id` = '1'");
}

// ========== INSERT Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLInsert) {
    std::vector<std::string> columns = {"name", "email"};
    std::vector<std::string> values = {"Alice", "alice@example.com"};
    std::string sql = postgresBuilder->insert("users", columns, values);
    EXPECT_EQ(sql,
              "INSERT INTO \"users\" (\"name\", \"email\") VALUES ('Alice', 'alice@example.com')");
}

TEST_F(SQLBuilderTest, MySQLInsert) {
    std::vector<std::string> columns = {"name", "email"};
    std::vector<std::string> values = {"Bob", "bob@example.com"};
    std::string sql = mysqlBuilder->insert("users", columns, values);
    EXPECT_EQ(sql, "INSERT INTO `users` (`name`, `email`) VALUES ('Bob', 'bob@example.com')");
}

TEST_F(SQLBuilderTest, SQLiteInsert) {
    std::vector<std::string> columns = {"name", "email"};
    std::vector<std::string> values = {"Charlie", "charlie@example.com"};
    std::string sql = sqliteBuilder->insert("users", columns, values);
    EXPECT_EQ(
        sql,
        "INSERT INTO \"users\" (\"name\", \"email\") VALUES ('Charlie', 'charlie@example.com')");
}

// ========== DELETE Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLDelete) {
    std::vector<std::pair<std::string, std::string>> whereConditions = {{"id", "1"}};
    std::string sql = postgresBuilder->deleteFrom("users", whereConditions);
    EXPECT_EQ(sql, "DELETE FROM \"users\" WHERE \"id\" = '1'");
}

TEST_F(SQLBuilderTest, PostgreSQLDeleteMultipleConditions) {
    std::vector<std::pair<std::string, std::string>> whereConditions = {{"id", "1"},
                                                                        {"status", "inactive"}};
    std::string sql = postgresBuilder->deleteFrom("users", whereConditions);
    EXPECT_EQ(sql, "DELETE FROM \"users\" WHERE \"id\" = '1' AND \"status\" = 'inactive'");
}

TEST_F(SQLBuilderTest, MySQLDelete) {
    std::vector<std::pair<std::string, std::string>> whereConditions = {{"id", "1"}};
    std::string sql = mysqlBuilder->deleteFrom("users", whereConditions);
    EXPECT_EQ(sql, "DELETE FROM `users` WHERE `id` = '1'");
}

// ========== DROP TABLE Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLDropTableWithIfExists) {
    std::string sql = postgresBuilder->dropTable("users", true);
    EXPECT_EQ(sql, "DROP TABLE IF EXISTS \"users\"");
}

TEST_F(SQLBuilderTest, PostgreSQLDropTableWithoutIfExists) {
    std::string sql = postgresBuilder->dropTable("users", false);
    EXPECT_EQ(sql, "DROP TABLE \"users\"");
}

TEST_F(SQLBuilderTest, MySQLDropTable) {
    std::string sql = mysqlBuilder->dropTable("users", true);
    EXPECT_EQ(sql, "DROP TABLE IF EXISTS `users`");
}

TEST_F(SQLBuilderTest, SQLiteDropTable) {
    std::string sql = sqliteBuilder->dropTable("users", true);
    EXPECT_EQ(sql, "DROP TABLE IF EXISTS \"users\"");
}

// ========== ADD COLUMN Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLAddColumn) {
    Column col;
    col.name = "phone";
    col.type = "VARCHAR(20)";
    col.isNotNull = false;

    std::string sql = postgresBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE \"users\" ADD COLUMN \"phone\" VARCHAR(20)");
}

TEST_F(SQLBuilderTest, PostgreSQLAddColumnNotNull) {
    Column col;
    col.name = "email";
    col.type = "TEXT";
    col.isNotNull = true;

    std::string sql = postgresBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE \"users\" ADD COLUMN \"email\" TEXT NOT NULL");
}

TEST_F(SQLBuilderTest, MySQLAddColumn) {
    Column col;
    col.name = "phone";
    col.type = "VARCHAR(20)";

    std::string sql = mysqlBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE `users` ADD COLUMN `phone` VARCHAR(20)");
}

// ========== DROP COLUMN Tests ==========

TEST_F(SQLBuilderTest, PostgreSQLDropColumn) {
    std::string sql = postgresBuilder->dropColumn("users", "phone");
    EXPECT_EQ(sql, "ALTER TABLE \"users\" DROP COLUMN \"phone\"");
}

TEST_F(SQLBuilderTest, MySQLDropColumn) {
    std::string sql = mysqlBuilder->dropColumn("users", "phone");
    EXPECT_EQ(sql, "ALTER TABLE `users` DROP COLUMN `phone`");
}

TEST_F(SQLBuilderTest, SQLiteDropColumn) {
    std::string sql = sqliteBuilder->dropColumn("users", "phone");
    EXPECT_EQ(sql, "ALTER TABLE \"users\" DROP COLUMN \"phone\"");
}

// ========== Factory Tests ==========

TEST_F(SQLBuilderTest, FactoryCreatesCorrectBuilder) {
    auto pgBuilder = createSQLBuilder(DatabaseType::POSTGRESQL);
    auto myBuilder = createSQLBuilder(DatabaseType::MYSQL);
    auto liteBuilder = createSQLBuilder(DatabaseType::SQLITE);

    EXPECT_NE(pgBuilder, nullptr);
    EXPECT_NE(myBuilder, nullptr);
    EXPECT_NE(liteBuilder, nullptr);

    // Verify correct builder type by checking identifier quoting
    EXPECT_EQ(pgBuilder->quoteIdentifier("test"), "\"test\"");
    EXPECT_EQ(myBuilder->quoteIdentifier("test"), "`test`");
    EXPECT_EQ(liteBuilder->quoteIdentifier("test"), "\"test\"");
}

TEST_F(SQLBuilderTest, FactoryReturnsDefaultForUnsupportedType) {
    // Redis doesn't have a SQL builder, but factory returns SQLite as default
    auto redisBuilder = createSQLBuilder(DatabaseType::REDIS);
    EXPECT_NE(redisBuilder, nullptr);
    // Verify it's using SQLite-style quoting
    EXPECT_EQ(redisBuilder->quoteIdentifier("test"), "\"test\"");
}

// ========== Edge Cases ==========

TEST_F(SQLBuilderTest, QuoteStringWithSingleQuotes) {
    EXPECT_EQ(postgresBuilder->quoteString("O'Brien"), "'O''Brien'");
    EXPECT_EQ(mysqlBuilder->quoteString("O'Brien"), "'O''Brien'");
    EXPECT_EQ(sqliteBuilder->quoteString("O'Brien"), "'O''Brien'");
}

TEST_F(SQLBuilderTest, QuoteStringWithMultipleSingleQuotes) {
    EXPECT_EQ(postgresBuilder->quoteString("it's John's"), "'it''s John''s'");
}

TEST_F(SQLBuilderTest, EmptyTableName) {
    std::string sql = postgresBuilder->selectAll("", 10, 0);
    EXPECT_EQ(sql, "SELECT * FROM \"\" LIMIT 10 OFFSET 0");
}

TEST_F(SQLBuilderTest, InsertWithEmptyColumns) {
    std::vector<std::string> columns = {};
    std::vector<std::string> values = {};
    std::string sql = postgresBuilder->insert("users", columns, values);
    EXPECT_EQ(sql, "INSERT INTO \"users\" () VALUES ()");
}
