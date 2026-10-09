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
