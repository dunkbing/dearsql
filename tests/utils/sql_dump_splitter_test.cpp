#include "utils/sql_dump_splitter.hpp"

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

    std::vector<std::string> splitAll(const std::string& dump) {
        std::istringstream in(dump);
        std::vector<std::string> statements;
        SqlDumpSplitter::split(in, [&](const std::string& statement) {
            statements.push_back(statement);
            return true;
        });
        return statements;
    }

} // namespace

TEST(SqlDumpSplitter, SplitsOnSemicolons) {
    const auto statements = splitAll("SELECT 1; SELECT 2;");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "SELECT 1");
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(SqlDumpSplitter, EmitsTrailingStatementWithoutDelimiter) {
    const auto statements = splitAll("SELECT 1");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(SqlDumpSplitter, SkipsEmptyStatements) {
    EXPECT_TRUE(splitAll(";;\n;\n").empty());
}

TEST(SqlDumpSplitter, IgnoresSemicolonInsideSingleQuotedString) {
    const auto statements = splitAll("INSERT INTO t VALUES ('a;b');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('a;b')");
}

TEST(SqlDumpSplitter, HandlesBackslashEscapedQuote) {
    const auto statements = splitAll("INSERT INTO t VALUES ('it\\'s; fine');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('it\\'s; fine')");
}

TEST(SqlDumpSplitter, HandlesDoubledQuote) {
    const auto statements = splitAll("INSERT INTO t VALUES ('it''s; fine');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('it''s; fine')");
}

TEST(SqlDumpSplitter, TrailingBackslashDoesNotEscapeClosingQuote) {
    // '...\\' ends with an escaped backslash, so the quote does close.
    const auto statements = splitAll("INSERT INTO t VALUES ('c:\\\\'); SELECT 2;");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(SqlDumpSplitter, IgnoresSemicolonInsideBacktickIdentifier) {
    const auto statements = splitAll("SELECT `we;ird` FROM t;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT `we;ird` FROM t");
}

TEST(SqlDumpSplitter, StripsLineComments) {
    const auto statements = splitAll("-- a comment; not a statement\nSELECT 1;\n# another;\n");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(SqlDumpSplitter, DoubleDashRequiresWhitespaceToStartComment) {
    // "--" without trailing space is an operator, not a comment, in MySQL.
    const auto statements = splitAll("SELECT 1--2;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1--2");
}

TEST(SqlDumpSplitter, StripsBlockCommentsSpanningLines) {
    const auto statements = splitAll("/* multi;\n   line; */\nSELECT 1;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(SqlDumpSplitter, BlockCommentSeparatesAdjacentTokens) {
    // Dropping the comment outright would fuse these into "SELECT 1UNION".
    const auto statements = splitAll("SELECT 1/*c*/UNION SELECT 2;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1 UNION SELECT 2");
}

TEST(SqlDumpSplitter, KeepsConditionalComments) {
    const auto statements = splitAll("/*!40000 ALTER TABLE `t` DISABLE KEYS */;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "/*!40000 ALTER TABLE `t` DISABLE KEYS */");
}

TEST(SqlDumpSplitter, SemicolonInsideConditionalCommentDoesNotSplit) {
    const auto statements = splitAll("/*!50003 SET a=1; SET b=2 */;\nSELECT 9;");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "/*!50003 SET a=1; SET b=2 */");
    EXPECT_EQ(statements[1], "SELECT 9");
}

TEST(SqlDumpSplitter, HonoursDelimiterDirective) {
    const auto statements = splitAll("DELIMITER $$\n"
                                     "CREATE TRIGGER x BEGIN SET @a = 1; END$$\n"
                                     "DELIMITER ;\n"
                                     "SELECT 1;\n");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "CREATE TRIGGER x BEGIN SET @a = 1; END");
    EXPECT_EQ(statements[1], "SELECT 1");
}

TEST(SqlDumpSplitter, DelimiterDirectiveIsNeverEmitted) {
    for (const auto& statement : splitAll("DELIMITER ;;\nSELECT 1;;\nDELIMITER ;\n")) {
        EXPECT_EQ(statement.find("DELIMITER"), std::string::npos);
    }
}

TEST(SqlDumpSplitter, MultiCharacterDelimiter) {
    const auto statements = splitAll("DELIMITER ;;\nSELECT 1;;\nSELECT 2;;\n");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "SELECT 1");
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(SqlDumpSplitter, PreservesMultiLineStatementBody) {
    const auto statements = splitAll("CREATE TABLE t (\n  a INT,\n  b INT\n);");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "CREATE TABLE t (\n  a INT,\n  b INT\n)");
}

TEST(SqlDumpSplitter, StringSpanningLinesKeepsNewline) {
    const auto statements = splitAll("INSERT INTO t VALUES ('line1\nline2');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('line1\nline2')");
}

TEST(SqlDumpSplitter, StopsWhenCallbackReturnsFalse) {
    std::istringstream in("SELECT 1; SELECT 2; SELECT 3;");
    std::vector<std::string> statements;
    const bool completed = SqlDumpSplitter::split(in, [&](const std::string& statement) {
        statements.push_back(statement);
        return statements.size() < 2;
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(statements.size(), 2u);
}
