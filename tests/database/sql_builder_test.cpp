#include "database/sql_builder.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>

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
