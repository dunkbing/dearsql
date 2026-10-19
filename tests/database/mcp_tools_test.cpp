#include "mcp/db_tools.hpp"
#include "mcp/fuzzy.hpp"
#include <dearsql/factory.hpp>
#include <gtest/gtest.h>

namespace {

    class OneConnectionHost : public mcp::Host {
    public:
        explicit OneConnectionHost(std::shared_ptr<dearsql::IConnection> conn)
            : conn_(std::move(conn)) {}
        std::vector<mcp::ConnectionEntry> connections() override {
            return {{"shop", "sqlite", true}};
        }
        std::string connect(const std::string&) override {
            return "";
        }
        std::shared_ptr<dearsql::IConnection> connection(const std::string& name) override {
            return name == "shop" ? conn_ : nullptr;
        }

    private:
        std::shared_ptr<dearsql::IConnection> conn_;
    };

    class McpToolsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dearsql::ConnectionInfo info;
            info.type = dearsql::DatabaseType::SQLITE;
            info.path = ":memory:";
            info.name = "shop";
            conn = dearsql::makeConnection(info);
            ASSERT_TRUE(conn->open().first);
            auto r = conn->database()->execute(
                "CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT NOT NULL, created_at TEXT);"
                "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER REFERENCES "
                "users(id), "
                "total REAL);"
                "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 120) "
                "INSERT INTO users (email) SELECT 'u' || i || '@x.io' FROM n;",
                0);
            ASSERT_TRUE(r.success()) << r.errorMessage();
            host = std::make_unique<OneConnectionHost>(conn);
            server = std::make_unique<mcp::Server>(*host);
        }

        std::pair<std::string, bool> call(const std::string& tool, nlohmann::json args) {
            auto r = server->callTool(tool, args);
            return {r["content"][0]["text"].get<std::string>(), r["isError"].get<bool>()};
        }

        std::shared_ptr<dearsql::IConnection> conn;
        std::unique_ptr<OneConnectionHost> host;
        std::unique_ptr<mcp::Server> server;
    };

} // namespace

TEST(Fuzzy, RanksExactPrefixSubstringTypo) {
    EXPECT_EQ(fuzzy::score("user_id", "userId"), 1000);
    EXPECT_GT(fuzzy::score("user", "users"), fuzzy::score("user", "power_users"));
    EXPECT_GT(fuzzy::score("usres", "users"), 0); // typo
    EXPECT_EQ(fuzzy::score("invoice", "orders"), 0);
}

TEST_F(McpToolsTest, SearchFindsColumnsAcrossTablesWithTypos) {
    auto [text, err] = call("search_schema", {{"query", "ordr usr"}});
    ASSERT_FALSE(err) << text;
    EXPECT_NE(text.find("column orders.user_id"), std::string::npos) << text;

    auto [best, err2] = call("search_schema", {{"query", "users"}});
    EXPECT_NE(best.find("→ describe_table users"), std::string::npos) << best;
}

TEST_F(McpToolsTest, DescribeShowsKeysAndResolvesNearMisses) {
    auto [text, err] = call("describe_table", {{"table", "order"}});
    ASSERT_FALSE(err) << text;
    EXPECT_NE(text.find("closest match orders"), std::string::npos) << text;
    EXPECT_NE(text.find("→ users(id)"), std::string::npos) << text;

    auto [users, err2] = call("describe_table", {{"name", "users"}});
    EXPECT_NE(users.find("referenced by: orders(user_id)"), std::string::npos) << users;
}

TEST_F(McpToolsTest, QueryPagesThroughCursor) {
    auto [first, err] = call("run_query", {{"sql", "SELECT id, email FROM users ORDER BY id"},
                                           {"maxRows", "50"}}); // numbers may arrive as strings
    ASSERT_FALSE(err) << first;
    EXPECT_NE(first.find("rows 1-50 of 120"), std::string::npos) << first;
    const auto pos = first.find("cursor: ");
    ASSERT_NE(pos, std::string::npos);
    const std::string cursor = first.substr(pos + 8, first.find('\n', pos) - pos - 8);

    auto [second, err2] = call("run_query", {{"cursor", cursor}, {"maxRows", 100}});
    EXPECT_NE(second.find("rows 51-120 of 120"), std::string::npos) << second;
    EXPECT_EQ(second.find("cursor:"), std::string::npos);
}

TEST_F(McpToolsTest, QueryRejectsWritesAndSuggestsNames) {
    auto [write, err] = call("run_query", {{"sql", "DELETE FROM users"}});
    EXPECT_TRUE(err);
    EXPECT_NE(write.find("Rejected"), std::string::npos);

    auto [typo, err2] = call("run_query", {{"query", "SELECT * FROM user"}});
    EXPECT_TRUE(err2);
    EXPECT_NE(typo.find("Did you mean: users"), std::string::npos) << typo;

    // a column of another table: point there, never suggest the same name back
    auto [col, err3] = call("run_query", {{"sql", "SELECT user_id FROM users"}});
    EXPECT_TRUE(err3);
    EXPECT_EQ(col.find("Did you mean: user_id"), std::string::npos) << col;
    EXPECT_NE(col.find("user_id is a column of: orders"), std::string::npos) << col;
}

TEST_F(McpToolsTest, ServesJsonRpcWithInstructions) {
    auto init = server->handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
    EXPECT_NE(init["result"]["instructions"].get<std::string>().find("search_schema"),
              std::string::npos);
    auto list = server->handle({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    EXPECT_EQ(list["result"]["tools"].size(), 6u);
    EXPECT_TRUE(
        server->handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
}

TEST(Fuzzy, SwapIsOneEdit) {
    EXPECT_EQ(fuzzy::editDistance("nmae", "name"), 1u);
    EXPECT_GT(fuzzy::score("nmae", "name"), 0);
}

TEST_F(McpToolsTest, CursorFromAnotherToolOrQueryIsNotReused) {
    auto [tables, err] = call("list_tables", {{"maxResults", 1}});
    ASSERT_FALSE(err) << tables;
    const auto cursor = tables.substr(tables.rfind("cursor: ") + 8, 1);
    auto [wrong, err2] = call("run_query", {{"cursor", cursor}});
    EXPECT_TRUE(err2);
    EXPECT_NE(wrong.find("belongs to list_tables"), std::string::npos) << wrong;
    // new sql wins over a stale cursor
    auto [fresh, err3] = call("run_query", {{"sql", "SELECT 'other' AS q"}, {"cursor", cursor}});
    EXPECT_FALSE(err3) << fresh;
    EXPECT_NE(fresh.find("other"), std::string::npos) << fresh;
}

TEST_F(McpToolsTest, CellsEscapePipesAndShowBlobs) {
    auto [text, err] = call("run_query", {{"sql", "SELECT 'a|b' AS x, x'00ff' AS b"}});
    ASSERT_FALSE(err) << text;
    EXPECT_NE(text.find("a\\|b | 0x00FF"), std::string::npos) << text;
}

TEST_F(McpToolsTest, SearchToleratesOneBadWordAndNamesTheDatabase) {
    auto [text, err] = call("search_schema", {{"query", "usres emial"}});
    EXPECT_NE(text.find("users.email"), std::string::npos) << text;
    auto [partial, err2] = call("search_schema", {{"query", "emial zzzzqx"}});
    EXPECT_NE(partial.find("users.email"), std::string::npos) << partial;
    auto [none, err3] = call("search_schema", {{"query", "zzzzqx"}});
    EXPECT_NE(none.find("of shop"), std::string::npos) << none;
}

TEST_F(McpToolsTest, QuerySaysWhyAPageIsShortAndThatTheTotalIsCapped) {
    auto [text, err] = call("run_query", {{"sql", "WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL "
                                                  "SELECT n+1 FROM s WHERE n < 3000) SELECT n, "
                                                  "printf('%0150d', n) FROM s"},
                                          {"maxRows", 500}});
    ASSERT_FALSE(err) << text;
    EXPECT_NE(text.find("1000+ (only the first 1000 are fetched)"), std::string::npos) << text;
    EXPECT_NE(text.find("page cut at"), std::string::npos) << text;
    EXPECT_NE(text.find("cursor: "), std::string::npos) << text;
}

TEST_F(McpToolsTest, ConnectToAnUnknownUrlSaysWhy) {
    auto [text, err] = call("connect", {{"name", "postgres://me@localhost/nope"}});
    EXPECT_TRUE(err);
    EXPECT_NE(text.find("add it in DearSQL"), std::string::npos) << text;
}

TEST_F(McpToolsTest, WritesOnlyWithAllowWrites) {
    auto listed = [](mcp::Server& s) {
        return s.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})
                   .dump()
                   .find("execute_sql") != std::string::npos;
    };
    EXPECT_FALSE(listed(*server));
    EXPECT_TRUE(call("execute_sql", {{"sql", "DELETE FROM users"}}).second);

    mcp::Server writer(*host, true);
    EXPECT_TRUE(listed(writer));
    auto r = writer.callTool("execute_sql", {{"sql", "DELETE FROM users WHERE id <= 20"}});
    const auto text = r["content"][0]["text"].get<std::string>();
    ASSERT_FALSE(r["isError"].get<bool>()) << text;
    EXPECT_NE(text.find("20 rows affected"), std::string::npos) << text;
    r = writer.callTool("run_query", {{"sql", "DELETE FROM users"}});
    EXPECT_NE(r["content"][0]["text"].get<std::string>().find("Use execute_sql"),
              std::string::npos);
}

namespace {
    // the GUI's shape: every write is asked about, connections can be saved
    class AskingHost : public OneConnectionHost {
    public:
        using OneConnectionHost::OneConnectionHost;
        std::string answer; // "" = approve
        std::vector<std::string> asked;
        std::string approveWrite(const std::string& connection, const std::string& sql) override {
            asked.push_back(connection + ": " + sql);
            return answer;
        }
        bool canSaveConnections() override {
            return true;
        }
        std::string saveConnection(const std::string& spec, const std::string& name,
                                   std::string& error) override {
            if (spec.starts_with("postgres://"))
                return name.empty() ? "shop" : name; // pretend saved, then "shop" opens
            error = "the user rejected it";
            return "";
        }
    };
} // namespace

TEST_F(McpToolsTest, WritesWaitForTheUserAndAddConnectionSaves) {
    AskingHost asking(conn);
    mcp::Server s(asking, true);
    auto text = [](const nlohmann::json& r) { return r["content"][0]["text"].get<std::string>(); };

    asking.answer = "the user rejected it";
    auto r = s.callTool("execute_sql", {{"sql", "DELETE FROM users WHERE id = 1"}});
    EXPECT_TRUE(r["isError"].get<bool>());
    EXPECT_NE(text(r).find("Not run: the user rejected it"), std::string::npos) << text(r);
    ASSERT_EQ(asking.asked.size(), 1u);
    EXPECT_EQ(asking.asked[0], "shop: DELETE FROM users WHERE id = 1");

    asking.answer = "";
    r = s.callTool("execute_sql", {{"sql", "DELETE FROM users WHERE id = 1"}});
    EXPECT_NE(text(r).find("1 rows affected"), std::string::npos) << text(r);

    auto listed = s.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}).dump();
    EXPECT_NE(listed.find("add_connection"), std::string::npos);
    r = s.callTool("add_connection", {{"url", "postgres://me@localhost/shop"}});
    EXPECT_NE(text(r).find("Saved and connected to shop"), std::string::npos) << text(r);
    r = s.callTool("add_connection", {{"url", "/nope.db"}});
    EXPECT_NE(text(r).find("Not saved: the user rejected it"), std::string::npos) << text(r);

    // the cli host can't save, so the tool isn't there
    listed = server->handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}).dump();
    EXPECT_EQ(listed.find("add_connection"), std::string::npos);
}
