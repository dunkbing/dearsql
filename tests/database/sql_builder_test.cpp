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
        mssqlBuilder = createSQLBuilder(DatabaseType::MSSQL);
        oracleBuilder = createSQLBuilder(DatabaseType::ORACLE);
    }

    std::unique_ptr<ISQLBuilder> postgresBuilder;
    std::unique_ptr<ISQLBuilder> mysqlBuilder;
    std::unique_ptr<ISQLBuilder> sqliteBuilder;
    std::unique_ptr<ISQLBuilder> mssqlBuilder;
    std::unique_ptr<ISQLBuilder> oracleBuilder;
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

// ========== Auto-Increment Tests ==========

TEST_F(SQLBuilderTest, MySQLAddColumnAutoIncrement) {
    Column col;
    col.name = "id";
    col.type = "INT";
    col.isNotNull = true;
    col.isAutoIncrement = true;

    std::string sql = mysqlBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE `users` ADD COLUMN `id` INT NOT NULL AUTO_INCREMENT");
}

TEST_F(SQLBuilderTest, MSSQLAddColumnIdentity) {
    Column col;
    col.name = "id";
    col.type = "INT";
    col.isNotNull = true;
    col.isAutoIncrement = true;

    std::string sql = mssqlBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE [users] ADD [id] INT IDENTITY(1,1) NOT NULL");
}

TEST_F(SQLBuilderTest, OracleAddColumnIdentity) {
    Column col;
    col.name = "id";
    col.type = "NUMBER";
    col.isAutoIncrement = true;

    std::string sql = oracleBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE \"users\" ADD \"id\" NUMBER GENERATED ALWAYS AS IDENTITY");
}

TEST_F(SQLBuilderTest, PostgreSQLAddColumnSerial) {
    Column col;
    col.name = "id";
    col.type = "INTEGER";
    col.isAutoIncrement = true;

    std::string sql = postgresBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE \"users\" ADD COLUMN \"id\" SERIAL");
}

TEST_F(SQLBuilderTest, PostgreSQLAddColumnBigserial) {
    Column col;
    col.name = "id";
    col.type = "BIGINT";
    col.isAutoIncrement = true;

    std::string sql = postgresBuilder->addColumn("users", col);
    EXPECT_EQ(sql, "ALTER TABLE \"users\" ADD COLUMN \"id\" BIGSERIAL");
}

TEST_F(SQLBuilderTest, MySQLCreateTableAutoIncrement) {
    Table table;
    table.name = "orders";

    Column idCol;
    idCol.name = "id";
    idCol.type = "INT";
    idCol.isPrimaryKey = true;
    idCol.isAutoIncrement = true;
    table.columns.push_back(idCol);

    Column nameCol;
    nameCol.name = "name";
    nameCol.type = "VARCHAR(255)";
    table.columns.push_back(nameCol);

    std::string sql = mysqlBuilder->createTable(table);
    EXPECT_NE(sql.find("AUTO_INCREMENT"), std::string::npos);
    EXPECT_NE(sql.find("PRIMARY KEY"), std::string::npos);
}

TEST_F(SQLBuilderTest, SQLiteCreateTableAutoIncrement) {
    Table table;
    table.name = "orders";

    Column idCol;
    idCol.name = "id";
    idCol.type = "INTEGER";
    idCol.isPrimaryKey = true;
    idCol.isAutoIncrement = true;
    table.columns.push_back(idCol);

    Column nameCol;
    nameCol.name = "name";
    nameCol.type = "TEXT";
    table.columns.push_back(nameCol);

    std::string sql = sqliteBuilder->createTable(table);
    // must use inline PRIMARY KEY AUTOINCREMENT
    EXPECT_NE(sql.find("PRIMARY KEY AUTOINCREMENT"), std::string::npos);
    // must not have a separate trailing PRIMARY KEY clause for this column
    // the only PRIMARY KEY should be inline
    auto firstPk = sql.find("PRIMARY KEY");
    auto secondPk = sql.find("PRIMARY KEY", firstPk + 1);
    EXPECT_EQ(secondPk, std::string::npos);
}

TEST_F(SQLBuilderTest, SQLiteCreateTableAutoIncrementIgnoresTrailingPrimaryKeyColumns) {
    Table table;
    table.name = "orders";

    Column codeCol;
    codeCol.name = "code";
    codeCol.type = "TEXT";
    codeCol.isPrimaryKey = true;
    table.columns.push_back(codeCol);

    Column idCol;
    idCol.name = "id";
    idCol.type = "INTEGER";
    idCol.isPrimaryKey = true;
    idCol.isAutoIncrement = true;
    table.columns.push_back(idCol);

    Column nameCol;
    nameCol.name = "name";
    nameCol.type = "TEXT";
    nameCol.isPrimaryKey = true;
    table.columns.push_back(nameCol);

    std::string sql = sqliteBuilder->createTable(table);
    EXPECT_NE(sql.find("\"id\" INTEGER PRIMARY KEY AUTOINCREMENT"), std::string::npos);

    auto firstPk = sql.find("PRIMARY KEY");
    auto secondPk = sql.find("PRIMARY KEY", firstPk + 1);
    EXPECT_EQ(secondPk, std::string::npos);
}

TEST_F(SQLBuilderTest, PostgreSQLCreateTableSerial) {
    Table table;
    table.name = "orders";

    Column idCol;
    idCol.name = "id";
    idCol.type = "INTEGER";
    idCol.isPrimaryKey = true;
    idCol.isAutoIncrement = true;
    table.columns.push_back(idCol);

    std::string sql = postgresBuilder->createTable(table);
    EXPECT_NE(sql.find("SERIAL"), std::string::npos);
    // should not contain the original INTEGER type
    EXPECT_EQ(sql.find("INTEGER"), std::string::npos);
}

// ========== supportsAutoIncrement Tests ==========

TEST(AutoIncrementSupportTest, SQLiteOnlySupportsInteger) {
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::SQLITE, "INTEGER"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::SQLITE, "TEXT"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::SQLITE, "REAL"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::SQLITE, "BIGINT"));
}

TEST(AutoIncrementSupportTest, MySQLSupportsIntegerTypes) {
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::MYSQL, "INT"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::MYSQL, "BIGINT"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::MYSQL, "INT(11)"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::MYSQL, "BIGINT(20)"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::MYSQL, "SMALLINT"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::MYSQL, "TINYINT"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::MYSQL, "VARCHAR(255)"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::MYSQL, "TEXT"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::MYSQL, "NUMBER"));
}

TEST(AutoIncrementSupportTest, PostgreSQLSupportsIntegerTypes) {
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::POSTGRESQL, "INTEGER"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::POSTGRESQL, "BIGINT"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::POSTGRESQL, "UUID"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::POSTGRESQL, "TEXT"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::POSTGRESQL, "NUMBER"));
}

TEST(AutoIncrementSupportTest, OracleSupportsNumberType) {
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::ORACLE, "NUMBER"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::ORACLE, "NUMBER(10)"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::ORACLE, "NUMBER(38,0)"));
    EXPECT_TRUE(supportsAutoIncrement(DatabaseType::ORACLE, "INTEGER"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::ORACLE, "VARCHAR2(255)"));
    EXPECT_FALSE(supportsAutoIncrement(DatabaseType::ORACLE, "CLOB"));
}
