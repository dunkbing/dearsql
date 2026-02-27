#include "ui/text_editor.hpp"

#include <gtest/gtest.h>
#include <string>

using dearsql::TextEditor;

// ========== Basic SELECT Tests ==========

TEST(SQLFormatTest, BasicSelect) {
    auto result = TextEditor::FormatSQL("select id, name from users");
    // Commas always split columns onto separate lines
    EXPECT_EQ(result, "SELECT id,\n"
                      "    name\n"
                      "FROM users\n");
}

TEST(SQLFormatTest, SelectMultipleColumns) {
    auto result = TextEditor::FormatSQL("select id, name, email, created_at from users");
    EXPECT_EQ(result, "SELECT id,\n"
                      "    name,\n"
                      "    email,\n"
                      "    created_at\n"
                      "FROM users\n");
}

TEST(SQLFormatTest, SelectStar) {
    auto result = TextEditor::FormatSQL("select * from users");
    EXPECT_EQ(result, "SELECT *\nFROM users\n");
}

TEST(SQLFormatTest, SelectDistinct) {
    auto result = TextEditor::FormatSQL("select distinct name from users");
    EXPECT_EQ(result, "SELECT DISTINCT name\nFROM users\n");
}

// ========== WHERE Clause Tests ==========

TEST(SQLFormatTest, WhereSimple) {
    auto result = TextEditor::FormatSQL("select id from users where active = true");
    EXPECT_EQ(result, "SELECT id\n"
                      "FROM users\n"
                      "WHERE active = TRUE\n");
}

TEST(SQLFormatTest, WhereWithAnd) {
    auto result =
        TextEditor::FormatSQL("select id from users where active = true and role = 'admin'");
    EXPECT_EQ(result, "SELECT id\n"
                      "FROM users\n"
                      "WHERE active = TRUE\n"
                      "    AND role = 'admin'\n");
}

TEST(SQLFormatTest, WhereWithAndOr) {
    auto result = TextEditor::FormatSQL(
        "select id from users where active = true and role = 'admin' or role = 'superadmin'");
    EXPECT_EQ(result, "SELECT id\n"
                      "FROM users\n"
                      "WHERE active = TRUE\n"
                      "    AND role = 'admin'\n"
                      "    OR role = 'superadmin'\n");
}

// ========== JOIN Tests ==========

TEST(SQLFormatTest, InnerJoin) {
    auto result = TextEditor::FormatSQL(
        "select u.id, o.total from users u join orders o on u.id = o.user_id");
    EXPECT_EQ(result, "SELECT u.id,\n"
                      "    o.total\n"
                      "FROM users u\n"
                      "JOIN orders o ON u.id = o.user_id\n");
}

TEST(SQLFormatTest, MultipleJoins) {
    auto result =
        TextEditor::FormatSQL("select u.id from users u join orders o on u.id = o.user_id "
                              "join order_items oi on o.id = oi.order_id where u.active = true");
    EXPECT_EQ(result, "SELECT u.id\n"
                      "FROM users u\n"
                      "JOIN orders o ON u.id = o.user_id\n"
                      "JOIN order_items oi ON o.id = oi.order_id\n"
                      "WHERE u.active = TRUE\n");
}

// ========== GROUP BY / ORDER BY Tests ==========

TEST(SQLFormatTest, GroupBy) {
    auto result = TextEditor::FormatSQL("select name, count(*) from users group by name");
    EXPECT_EQ(result, "SELECT name,\n"
                      "    COUNT(*)\n"
                      "FROM users\n"
                      "GROUP BY name\n");
}

TEST(SQLFormatTest, OrderByLimit) {
    auto result = TextEditor::FormatSQL("select id from users order by id limit 10");
    EXPECT_EQ(result, "SELECT id\n"
                      "FROM users\n"
                      "ORDER BY id\n"
                      "LIMIT 10\n");
}

TEST(SQLFormatTest, FullQuery) {
    auto result = TextEditor::FormatSQL(
        "select id, name from users u join orders o on u.id = o.user_id "
        "where active = true and role = 'admin' group by name order by name limit 10");
    EXPECT_EQ(result, "SELECT id,\n"
                      "    name\n"
                      "FROM users u\n"
                      "JOIN orders o ON u.id = o.user_id\n"
                      "WHERE active = TRUE\n"
                      "    AND role = 'admin'\n"
                      "GROUP BY name\n"
                      "ORDER BY name\n"
                      "LIMIT 10\n");
}

// ========== Subquery Tests ==========

TEST(SQLFormatTest, SubqueryInFrom) {
    auto result = TextEditor::FormatSQL("select * from (select id from users) sub where id > 5");
    EXPECT_EQ(result, "SELECT *\n"
                      "FROM (\n"
                      "    SELECT id FROM users\n"
                      ") sub\n"
                      "WHERE id > 5\n");
}

// ========== CTE Tests ==========

TEST(SQLFormatTest, SimpleCTE) {
    auto result = TextEditor::FormatSQL("with cte as (select 1) select * from cte");
    EXPECT_EQ(result, "WITH cte AS (\n"
                      "SELECT 1)\n"
                      "SELECT *\n"
                      "FROM cte\n");
}

// ========== CASE Expression Tests ==========

TEST(SQLFormatTest, CaseExpression) {
    auto result = TextEditor::FormatSQL("select case when x > 1 then 'a' else 'b' end from t");
    EXPECT_EQ(result, "SELECT CASE\n"
                      "    WHEN x > 1 THEN 'a'\n"
                      "    ELSE 'b'\n"
                      "END\n"
                      "FROM t\n");
}

// ========== Multiple Statements ==========

TEST(SQLFormatTest, MultipleStatements) {
    auto result = TextEditor::FormatSQL("select 1; select 2;");
    EXPECT_EQ(result, "SELECT 1;\n"
                      "SELECT 2;\n");
}

// ========== Edge Cases ==========

TEST(SQLFormatTest, EmptyInput) {
    auto result = TextEditor::FormatSQL("");
    EXPECT_EQ(result, "");
}

TEST(SQLFormatTest, WhitespaceOnly) {
    auto result = TextEditor::FormatSQL("   \n\t  ");
    // Parser should handle whitespace-only input
    EXPECT_TRUE(result.empty() || result == "\n");
}

TEST(SQLFormatTest, KeywordsUppercased) {
    auto result = TextEditor::FormatSQL("select id from users where id = 1");
    EXPECT_TRUE(result.find("SELECT") != std::string::npos);
    EXPECT_TRUE(result.find("FROM") != std::string::npos);
    EXPECT_TRUE(result.find("WHERE") != std::string::npos);
    // Lowercase keywords should not appear
    EXPECT_EQ(result.find("select"), std::string::npos);
    EXPECT_EQ(result.find("from"), std::string::npos);
    EXPECT_EQ(result.find("where"), std::string::npos);
}

TEST(SQLFormatTest, PreservesIdentifiers) {
    auto result = TextEditor::FormatSQL("select my_column from my_table");
    EXPECT_TRUE(result.find("my_column") != std::string::npos);
    EXPECT_TRUE(result.find("my_table") != std::string::npos);
}

TEST(SQLFormatTest, PreservesStringLiterals) {
    auto result = TextEditor::FormatSQL("select * from users where name = 'hello world'");
    EXPECT_TRUE(result.find("'hello world'") != std::string::npos);
}

// ========== INSERT / UPDATE / DELETE ==========

TEST(SQLFormatTest, InsertStatement) {
    auto result = TextEditor::FormatSQL(
        "insert into users (name, email) values ('John', 'john@example.com')");
    EXPECT_TRUE(result.find("INSERT") != std::string::npos);
    EXPECT_TRUE(result.find("INTO") != std::string::npos);
    EXPECT_TRUE(result.find("VALUES") != std::string::npos);
}

TEST(SQLFormatTest, UpdateStatement) {
    auto result = TextEditor::FormatSQL("update users set name = 'Jane' where id = 1");
    EXPECT_TRUE(result.find("UPDATE") != std::string::npos);
    EXPECT_TRUE(result.find("SET") != std::string::npos);
    EXPECT_TRUE(result.find("WHERE") != std::string::npos);
}

TEST(SQLFormatTest, DeleteStatement) {
    auto result = TextEditor::FormatSQL("delete from users where id = 1");
    EXPECT_TRUE(result.find("DELETE") != std::string::npos);
    EXPECT_TRUE(result.find("FROM") != std::string::npos);
    EXPECT_TRUE(result.find("WHERE") != std::string::npos);
}

// ========== Function Call Tests ==========

TEST(SQLFormatTest, FunctionCallUppercased) {
    auto result = TextEditor::FormatSQL("select count(*) from users");
    EXPECT_TRUE(result.find("COUNT(*)") != std::string::npos);
}

TEST(SQLFormatTest, FunctionCallMultipleArgs) {
    auto result = TextEditor::FormatSQL("select coalesce(a, b) from t");
    EXPECT_TRUE(result.find("COALESCE(a, b)") != std::string::npos);
}

TEST(SQLFormatTest, FunctionCallNested) {
    auto result = TextEditor::FormatSQL("select lower(trim(name)) from t");
    EXPECT_TRUE(result.find("LOWER(TRIM(name))") != std::string::npos);
}

// ========== Operator Spacing ==========

TEST(SQLFormatTest, OperatorSpacing) {
    auto result = TextEditor::FormatSQL("select * from t where a=1 and b<>2 and c>=3");
    EXPECT_TRUE(result.find("a = 1") != std::string::npos);
    EXPECT_TRUE(result.find("b <> 2") != std::string::npos);
    EXPECT_TRUE(result.find("c >= 3") != std::string::npos);
}
