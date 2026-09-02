#include "utils/sql_guard.hpp"
#include <gtest/gtest.h>

TEST(SqlGuardTest, AllowsReadOnlyStatements) {
    EXPECT_TRUE(SqlGuard::isReadOnly("SELECT * FROM users"));
    EXPECT_TRUE(SqlGuard::isReadOnly("  select 1"));
    EXPECT_TRUE(SqlGuard::isReadOnly("WITH t AS (SELECT 1) SELECT * FROM t"));
    EXPECT_TRUE(SqlGuard::isReadOnly("EXPLAIN SELECT 1"));
    EXPECT_TRUE(SqlGuard::isReadOnly("SHOW CREATE TABLE users"));
    EXPECT_TRUE(SqlGuard::isReadOnly("SELECT REPLACE(name, 'a', 'b') FROM users"));
    EXPECT_TRUE(SqlGuard::isReadOnly("SELECT updated_at, delete_reason FROM audit"));
    EXPECT_TRUE(SqlGuard::isReadOnly("SHOW TABLES"));
    EXPECT_TRUE(SqlGuard::isReadOnly("PRAGMA table_info(users)"));
    EXPECT_TRUE(SqlGuard::isReadOnly("SELECT 1; SELECT 2;"));
}

TEST(SqlGuardTest, RejectsWrites) {
    EXPECT_FALSE(SqlGuard::isReadOnly("DELETE FROM users"));
    EXPECT_FALSE(SqlGuard::isReadOnly("UPDATE users SET name = 'x'"));
    EXPECT_FALSE(SqlGuard::isReadOnly("INSERT INTO users VALUES (1)"));
    EXPECT_FALSE(SqlGuard::isReadOnly("DROP TABLE users"));
    EXPECT_FALSE(SqlGuard::isReadOnly("TRUNCATE users"));
    EXPECT_FALSE(SqlGuard::isReadOnly("GRANT ALL ON users TO bob"));
}

TEST(SqlGuardTest, RejectsWriteHiddenAfterReadStatement) {
    EXPECT_FALSE(SqlGuard::isReadOnly("SELECT 1; DROP TABLE users"));
    EXPECT_FALSE(SqlGuard::isReadOnly("SELECT 1;\n-- comment\nDELETE FROM t"));
}

TEST(SqlGuardTest, SkipsLeadingComments) {
    EXPECT_TRUE(SqlGuard::isReadOnly("-- a comment\nSELECT 1"));
    EXPECT_TRUE(SqlGuard::isReadOnly("/* block */ SELECT 1"));
    EXPECT_FALSE(SqlGuard::isReadOnly("/* sneaky */ DELETE FROM t"));
}

TEST(SqlGuardTest, SemicolonInsideStringIsNotAStatementBreak) {
    EXPECT_TRUE(SqlGuard::isReadOnly("SELECT ';DROP TABLE users' AS x"));
}

TEST(SqlGuardTest, RejectsWritesHiddenInReadStatements) {
    EXPECT_FALSE(SqlGuard::isReadOnly("EXPLAIN ANALYZE DELETE FROM users"));
    EXPECT_FALSE(SqlGuard::isReadOnly("EXPLAIN (ANALYZE, BUFFERS) DELETE FROM users"));
    EXPECT_FALSE(SqlGuard::isReadOnly("WITH d AS (DELETE FROM users RETURNING *) SELECT * FROM d"));
    EXPECT_FALSE(SqlGuard::isReadOnly("SELECT * INTO backup FROM users"));
    // quote inside a comment must not swallow the separator
    EXPECT_FALSE(SqlGuard::isReadOnly("SELECT 1 -- '\n; DELETE FROM t"));
    EXPECT_FALSE(SqlGuard::isReadOnly("SELECT 1 /* ' */; DELETE FROM t"));
}

TEST(SqlGuardTest, RejectsEmptyInput) {
    EXPECT_FALSE(SqlGuard::isReadOnly(""));
    EXPECT_FALSE(SqlGuard::isReadOnly("   \n  "));
    EXPECT_FALSE(SqlGuard::isReadOnly("-- only a comment"));
}
