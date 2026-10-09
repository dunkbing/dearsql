#include "cli/lsp.hpp"
#include <dearsql/factory.hpp>
#include <filesystem>
#include <gtest/gtest.h>

namespace {

    using json = nlohmann::json;

    class LspTest : public ::testing::Test {
    protected:
        void SetUp() override {
            path_ =
                (std::filesystem::temp_directory_path() /
                 ("dearsql_lsp_test_" +
                  std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".sqlite"))
                    .string();
            std::filesystem::remove(path_);
            dearsql::ConnectionInfo info;
            info.type = dearsql::DatabaseType::SQLITE;
            info.path = path_;
            auto conn = dearsql::makeConnection(info);
            ASSERT_TRUE(conn->open().first);
            auto db = conn->database();
            ASSERT_TRUE(db->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT "
                                    "NULL, email TEXT);"
                                    "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id "
                                    "INTEGER REFERENCES users(id), total REAL);")
                            .success());
            conn->close();
        }
        void TearDown() override {
            server_.reset();
            conns_.reset();
            std::filesystem::remove(path_);
        }

        LspServer& start(const std::string& arg = "", json initOptions = nullptr) {
            conns_ = std::make_unique<CliConnections>();
            std::string name;
            if (!arg.empty()) {
                std::string error;
                name = conns_->add(arg, error);
                EXPECT_FALSE(name.empty()) << error;
            }
            server_ = std::make_unique<LspServer>(*conns_, name,
                                                  [this](const json& m) { sent_.push_back(m); });
            json params = {{"processId", nullptr}, {"capabilities", json::object()}};
            if (!initOptions.is_null())
                params["initializationOptions"] = initOptions;
            request("initialize", params);
            server_->handle({{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", {}}});
            return *server_;
        }
        json request(const std::string& method, json params) {
            const int id = nextId_++;
            sent_.clear();
            server_->handle(
                {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
            for (const auto& m : sent_) {
                if (m.contains("id") && m["id"] == id)
                    return m;
            }
            return nullptr;
        }
        void open(const std::string& text) {
            server_->handle(
                {{"jsonrpc", "2.0"},
                 {"method", "textDocument/didOpen"},
                 {"params",
                  {{"textDocument",
                    {{"uri", URI}, {"languageId", "sql"}, {"version", 1}, {"text", text}}}}}});
        }
        json at(int line, int character) {
            return {{"textDocument", {{"uri", URI}}},
                    {"position", {{"line", line}, {"character", character}}}};
        }
        static const json* item(const json& completion, const std::string& label) {
            for (const auto& i : completion["result"]["items"]) {
                if (i["label"] == label)
                    return &i;
            }
            return nullptr;
        }

        static constexpr const char* URI = "file:///tmp/query.sql";
        std::string path_;
        std::unique_ptr<CliConnections> conns_;
        std::unique_ptr<LspServer> server_;
        std::vector<json> sent_;
        int nextId_ = 1;
    };

} // namespace

TEST(LspPositions, Utf16) {
    const std::string text = "-- é😀x\nSELECT 'ü' AS a";
    // "-- " 3 units, é 1, 😀 2 -> x at character 6, byte 3 + 2 + 4 = 9
    EXPECT_EQ(lspOffsetAt(text, 0, 6), 9u);
    EXPECT_EQ(lspPositionAt(text, 9), std::make_pair(0, 6));
    const size_t a = text.find(" a");
    EXPECT_EQ(lspPositionAt(text, a), std::make_pair(1, 13));
    EXPECT_EQ(lspOffsetAt(text, 1, 13), a);
    EXPECT_EQ(lspOffsetAt(text, 0, 99), text.find('\n'));
    EXPECT_EQ(lspOffsetAt(text, 9, 0), text.size());
}

TEST_F(LspTest, InitializeAdvertisesCapabilities) {
    start();
    auto r = request("initialize", {{"capabilities", json::object()}});
    const auto& caps = r["result"]["capabilities"];
    EXPECT_EQ(caps["textDocumentSync"], 1);
    EXPECT_EQ(caps["completionProvider"]["triggerCharacters"], json::array({"."}));
    EXPECT_TRUE(caps["hoverProvider"].get<bool>());
    EXPECT_EQ(r["result"]["serverInfo"]["name"], "dearsql");
}

TEST_F(LspTest, CompletesAliasColumnsFromFileComment) {
    start();
    open("-- dearsql: " + path_ + "\nSELECT  FROM users u WHERE u.");
    auto r = request("textDocument/completion", at(1, 29));
    const auto* email = item(r, "email");
    ASSERT_NE(email, nullptr) << r.dump(2);
    EXPECT_EQ((*email)["kind"], 5);
    EXPECT_EQ((*email)["textEdit"]["range"]["start"]["line"], 1);
    EXPECT_EQ((*email)["textEdit"]["range"]["start"]["character"], 29);
    EXPECT_EQ((*email)["textEdit"]["newText"], "email");
    EXPECT_NE((*email)["detail"].get<std::string>().find("TEXT"), std::string::npos);
    EXPECT_NE((*email)["documentation"]["value"].get<std::string>().find("users.email"),
              std::string::npos);
    EXPECT_EQ(item(r, "total"), nullptr); // orders is not in scope
    EXPECT_EQ(item(r, "SELECT"), nullptr);
}

TEST_F(LspTest, CompletesTablesFromCommandLineConnection) {
    start(path_);
    open("SELECT * FROM us");
    auto r = request("textDocument/completion", at(0, 16));
    const auto* users = item(r, "users");
    ASSERT_NE(users, nullptr) << r.dump(2);
    EXPECT_EQ((*users)["kind"], 7);
    EXPECT_EQ((*users)["textEdit"]["range"]["start"]["character"], 14);
    EXPECT_NE((*users)["documentation"]["value"].get<std::string>().find("email"),
              std::string::npos);
}

TEST_F(LspTest, InitializationOptionsPickTheConnection) {
    start("", {{"connection", path_}});
    open("SELECT  FROM orders");
    auto r = request("textDocument/completion", at(0, 7));
    EXPECT_NE(item(r, "total"), nullptr) << r.dump(2);
}

TEST_F(LspTest, HoverShowsTableColumnsAndColumnFacts) {
    start(path_);
    open("SELECT o.user_id FROM orders o JOIN users u ON u.id = o.user_id");
    auto table = request("textDocument/hover", at(0, 38)); // users
    ASSERT_TRUE(table["result"].is_object()) << table.dump(2);
    const auto text = table["result"]["contents"]["value"].get<std::string>();
    EXPECT_NE(text.find("**table** `users`"), std::string::npos) << text;
    EXPECT_NE(text.find("`email` TEXT"), std::string::npos) << text;
    EXPECT_NE(text.find("primary key"), std::string::npos) << text;

    auto column = request("textDocument/hover", at(0, 10)); // o.user_id
    const auto facts = column["result"]["contents"]["value"].get<std::string>();
    EXPECT_NE(facts.find("orders.user_id"), std::string::npos) << facts;
    EXPECT_NE(facts.find("nullable"), std::string::npos) << facts;
    EXPECT_NE(facts.find("→ users(id)"), std::string::npos) << facts;

    EXPECT_TRUE(request("textDocument/hover", at(0, 2))["result"].is_null()); // SELECT
}

TEST_F(LspTest, FullSyncChangesAreSeen) {
    start(path_);
    open("SELECT 1");
    server_->handle({{"jsonrpc", "2.0"},
                     {"method", "textDocument/didChange"},
                     {"params",
                      {{"textDocument", {{"uri", URI}, {"version", 2}}},
                       {"contentChanges", {{{"text", "SELECT  FROM orders"}}}}}}});
    EXPECT_NE(item(request("textDocument/completion", at(0, 7)), "total"), nullptr);
}

TEST_F(LspTest, FailedConnectionWarnsOnceAndCompletesKeywords) {
    start();
    open("-- dearsql: no-such-connection\nSEL");
    auto r = request("textDocument/completion", at(1, 3));
    EXPECT_NE(item(r, "SELECT"), nullptr) << r.dump(2);
    int warnings = 0;
    for (const auto& m : sent_)
        warnings += m.value("method", "") == "window/showMessage";
    EXPECT_EQ(warnings, 1);
    request("textDocument/completion", at(1, 3));
    for (const auto& m : sent_)
        EXPECT_NE(m.value("method", ""), "window/showMessage");
}

TEST_F(LspTest, SqlsStyleCommandsSwitchTheTarget) {
    start();
    open("SELECT  FROM orders");
    EXPECT_EQ(item(request("textDocument/completion", at(0, 7)), "total"), nullptr);
    auto sw = request("workspace/executeCommand",
                      {{"command", "switchConnections"}, {"arguments", {path_}}});
    EXPECT_FALSE(sw.contains("error")) << sw.dump(2);
    EXPECT_NE(item(request("textDocument/completion", at(0, 7)), "total"), nullptr);
    auto tables = request("workspace/executeCommand", {{"command", "showTables"}});
    EXPECT_EQ(tables["result"], "orders\nusers\n");
    auto conns = request("workspace/executeCommand", {{"command", "showConnections"}});
    EXPECT_NE(conns["result"].get<std::string>().find(" *"), std::string::npos);
}

// a path from initializationOptions stays one entry across switchDatabase and
// shows as active
TEST_F(LspTest, PathTargetIsOneActiveEntry) {
    start("", {{"connection", path_}});
    open("SELECT  FROM orders");
    EXPECT_NE(item(request("textDocument/completion", at(0, 7)), "total"), nullptr);
    request("workspace/executeCommand", {{"command", "switchDatabase"}, {"arguments", {"main"}}});
    EXPECT_NE(item(request("textDocument/completion", at(0, 7)), "total"), nullptr);
    const auto list =
        request("workspace/executeCommand", {{"command", "showConnections"}})["result"]
            .get<std::string>();
    const auto file = std::filesystem::path(path_).filename().string();
    EXPECT_EQ(list, "1 sqlite " + file + " *\n");
}

TEST_F(LspTest, UnknownRequestsAndShutdown) {
    start();
    auto r = request("textDocument/definition", at(0, 0));
    EXPECT_EQ(r["error"]["code"], -32601);
    EXPECT_TRUE(server_->handle({{"jsonrpc", "2.0"}, {"method", "$/unknown"}}));
    EXPECT_EQ(server_->exitCode(), 1);
    EXPECT_TRUE(request("shutdown", nullptr)["result"].is_null());
    EXPECT_FALSE(server_->handle({{"jsonrpc", "2.0"}, {"method", "exit"}}));
    EXPECT_EQ(server_->exitCode(), 0);
}
