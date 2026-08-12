#include "utils/mysql_dump_splitter.hpp"

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

    std::vector<std::string> splitAll(const std::string& dump) {
        std::istringstream in(dump);
        std::vector<std::string> statements;
        MysqlDumpSplitter::split(in, [&](const std::string& statement, bool) {
            statements.push_back(statement);
            return true;
        });
        return statements;
    }

    std::vector<bool> compoundFlags(const std::string& dump) {
        std::istringstream in(dump);
        std::vector<bool> flags;
        MysqlDumpSplitter::split(in, [&](const std::string&, const bool compound) {
            flags.push_back(compound);
            return true;
        });
        return flags;
    }

} // namespace

TEST(MysqlDumpSplitter, SplitsOnSemicolons) {
    const auto statements = splitAll("SELECT 1; SELECT 2;");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "SELECT 1");
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(MysqlDumpSplitter, EmitsTrailingStatementWithoutDelimiter) {
    const auto statements = splitAll("SELECT 1");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(MysqlDumpSplitter, SkipsEmptyStatements) {
    EXPECT_TRUE(splitAll(";;\n;\n").empty());
}

TEST(MysqlDumpSplitter, IgnoresSemicolonInsideSingleQuotedString) {
    const auto statements = splitAll("INSERT INTO t VALUES ('a;b');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('a;b')");
}

TEST(MysqlDumpSplitter, HandlesBackslashEscapedQuote) {
    const auto statements = splitAll("INSERT INTO t VALUES ('it\\'s; fine');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('it\\'s; fine')");
}

TEST(MysqlDumpSplitter, HandlesDoubledQuote) {
    const auto statements = splitAll("INSERT INTO t VALUES ('it''s; fine');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('it''s; fine')");
}

TEST(MysqlDumpSplitter, TrailingBackslashDoesNotEscapeClosingQuote) {
    // '...\\' ends with an escaped backslash, so the quote does close.
    const auto statements = splitAll("INSERT INTO t VALUES ('c:\\\\'); SELECT 2;");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(MysqlDumpSplitter, IgnoresSemicolonInsideBacktickIdentifier) {
    const auto statements = splitAll("SELECT `we;ird` FROM t;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT `we;ird` FROM t");
}

TEST(MysqlDumpSplitter, StripsLineComments) {
    const auto statements = splitAll("-- a comment; not a statement\nSELECT 1;\n# another;\n");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(MysqlDumpSplitter, DoubleDashRequiresWhitespaceToStartComment) {
    // "--" without trailing space is an operator, not a comment, in MySQL.
    const auto statements = splitAll("SELECT 1--2;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1--2");
}

TEST(MysqlDumpSplitter, StripsBlockCommentsSpanningLines) {
    const auto statements = splitAll("/* multi;\n   line; */\nSELECT 1;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1");
}

TEST(MysqlDumpSplitter, BlockCommentSeparatesAdjacentTokens) {
    // Dropping the comment outright would fuse these into "SELECT 1UNION".
    const auto statements = splitAll("SELECT 1/*c*/UNION SELECT 2;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "SELECT 1 UNION SELECT 2");
}

TEST(MysqlDumpSplitter, KeepsConditionalComments) {
    const auto statements = splitAll("/*!40000 ALTER TABLE `t` DISABLE KEYS */;");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "/*!40000 ALTER TABLE `t` DISABLE KEYS */");
}

TEST(MysqlDumpSplitter, SemicolonInsideConditionalCommentDoesNotSplit) {
    const auto statements = splitAll("/*!50003 SET a=1; SET b=2 */;\nSELECT 9;");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "/*!50003 SET a=1; SET b=2 */");
    EXPECT_EQ(statements[1], "SELECT 9");
}

TEST(MysqlDumpSplitter, HonoursDelimiterDirective) {
    const auto statements = splitAll("DELIMITER $$\n"
                                     "CREATE TRIGGER x BEGIN SET @a = 1; END$$\n"
                                     "DELIMITER ;\n"
                                     "SELECT 1;\n");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "CREATE TRIGGER x BEGIN SET @a = 1; END");
    EXPECT_EQ(statements[1], "SELECT 1");
}

TEST(MysqlDumpSplitter, DelimiterDirectiveIsNeverEmitted) {
    for (const auto& statement : splitAll("DELIMITER ;;\nSELECT 1;;\nDELIMITER ;\n")) {
        EXPECT_EQ(statement.find("DELIMITER"), std::string::npos);
    }
}

TEST(MysqlDumpSplitter, MultiCharacterDelimiter) {
    const auto statements = splitAll("DELIMITER ;;\nSELECT 1;;\nSELECT 2;;\n");
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_EQ(statements[0], "SELECT 1");
    EXPECT_EQ(statements[1], "SELECT 2");
}

TEST(MysqlDumpSplitter, PreservesMultiLineStatementBody) {
    const auto statements = splitAll("CREATE TABLE t (\n  a INT,\n  b INT\n);");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "CREATE TABLE t (\n  a INT,\n  b INT\n)");
}

TEST(MysqlDumpSplitter, StringSpanningLinesKeepsNewline) {
    const auto statements = splitAll("INSERT INTO t VALUES ('line1\nline2');");
    ASSERT_EQ(statements.size(), 1u);
    EXPECT_EQ(statements[0], "INSERT INTO t VALUES ('line1\nline2')");
}

TEST(MysqlDumpSplitter, StopsWhenCallbackReturnsFalse) {
    std::istringstream in("SELECT 1; SELECT 2; SELECT 3;");
    std::vector<std::string> statements;
    const bool completed = MysqlDumpSplitter::split(in, [&](const std::string& statement, bool) {
        statements.push_back(statement);
        return statements.size() < 2;
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(statements.size(), 2u);
}

TEST(MysqlDumpSplitter, FlagsOnlyDelimiterBlockStatementsAsCompound) {
    const auto flags = compoundFlags("SELECT 1;\n"
                                     "DELIMITER $$\n"
                                     "CREATE TRIGGER x BEGIN SET @a = 1; END$$\n"
                                     "DELIMITER ;\n"
                                     "SELECT 2;\n");
    ASSERT_EQ(flags.size(), 3u);
    EXPECT_FALSE(flags[0]);
    EXPECT_TRUE(flags[1]);
    EXPECT_FALSE(flags[2]);
}
