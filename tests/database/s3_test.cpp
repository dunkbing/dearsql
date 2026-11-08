#include "database/s3.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include "mcp/db_tools.hpp"

namespace {
    // DEARSQL_TEST_S3_ENDPOINT=host:port (plain http), plus ..._ACCESS_KEY / ..._SECRET_KEY;
    // scripts/run-tests starts a VersityGW container for it
    std::optional<DatabaseConnectionInfo> s3InfoFromEnv() {
        const char* endpoint = std::getenv("DEARSQL_TEST_S3_ENDPOINT");
        const char* access = std::getenv("DEARSQL_TEST_S3_ACCESS_KEY");
        const char* secret = std::getenv("DEARSQL_TEST_S3_SECRET_KEY");
        if (!endpoint || !access || !secret)
            return std::nullopt;
        DatabaseConnectionInfo info;
        info.type = DatabaseType::S3;
        info.name = "S3Integration";
        std::string host = endpoint;
        const auto colon = host.rfind(':');
        info.host = host.substr(0, colon);
        info.port = colon == std::string::npos ? 80 : std::stoi(host.substr(colon + 1));
        info.sslmode = SslMode::Disable;
        info.username = access;
        info.password = secret;
        return info;
    }

    std::string readAll(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
} // namespace

TEST(S3Test, BucketObjectRoundTrip) {
    const auto info = s3InfoFromEnv();
    if (!info)
        GTEST_SKIP() << "set DEARSQL_TEST_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY";

    S3Database db(*info);
    auto [ok, err] = db.connect();
    ASSERT_TRUE(ok) << err;
    const auto client = db.client();

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string bucket = std::format("dearsql-test-{}", stamp % 1000000000);
    client->createBucket(bucket);

    const auto tmp = std::filesystem::temp_directory_path();
    const auto src = tmp / std::format("dearsql_s3_{}.txt", stamp);
    const auto dst = tmp / std::format("dearsql_s3_{}_down.txt", stamp);
    std::ofstream(src, std::ios::binary) << "hello s3\n";

    // a space and a folder in the key
    client->upload(bucket, "dir/a b.txt", src.string());
    client->upload(bucket, "top.txt", src.string());

    const auto root = client->listObjects(bucket, "");
    ASSERT_EQ(root.prefixes, std::vector<std::string>{"dir/"});
    ASSERT_EQ(root.objects.size(), 1u);
    EXPECT_EQ(root.objects[0].key, "top.txt");
    EXPECT_EQ(root.objects[0].size, 9u);

    const auto dir = client->listObjects(bucket, "dir/");
    ASSERT_EQ(dir.objects.size(), 1u);
    EXPECT_EQ(dir.objects[0].key, "dir/a b.txt");

    const auto details = client->stat(bucket, "dir/a b.txt");
    EXPECT_EQ(details.front().second, "9 bytes");

    client->download(bucket, "dir/a b.txt", dst.string());
    EXPECT_EQ(readAll(dst), "hello s3\n");

    EXPECT_TRUE(client->presignGet(bucket, "top.txt", 3600).starts_with("http://"));

    EXPECT_THROW(client->deleteBucket(bucket), S3Error); // not empty
    EXPECT_EQ(client->deletePrefix(bucket, "dir/"), 1);
    client->deleteObject(bucket, "top.txt");
    EXPECT_TRUE(client->listObjects(bucket, "").objects.empty());
    client->deleteBucket(bucket);

    std::filesystem::remove(src);
    std::filesystem::remove(dst);
}

// copy and move (copy + delete) as the browser's paste does them, across two buckets
TEST(S3Test, CopyObjectsAndFolders) {
    const auto info = s3InfoFromEnv();
    if (!info)
        GTEST_SKIP() << "set DEARSQL_TEST_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY";
    S3Database db(*info);
    auto [ok, err] = db.connect();
    ASSERT_TRUE(ok) << err;
    const auto client = db.client();

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000;
    const std::string a = std::format("dearsql-copy-a-{}", stamp);
    const std::string b = std::format("dearsql-copy-b-{}", stamp);
    client->createBucket(a);
    client->createBucket(b);
    const auto src = std::filesystem::temp_directory_path() / std::format("dearsql_s3c_{}", stamp);
    std::ofstream(src, std::ios::binary) << "copy me";
    client->upload(a, "one.txt", src.string());
    client->upload(a, "dir/x.txt", src.string());
    client->upload(a, "dir/sub/y.txt", src.string());

    client->copyObject(a, "one.txt", a, "one copy.txt"); // same bucket
    EXPECT_EQ(client->copyPrefix(a, "dir/", b, "moved/"), 2); // another bucket
    client->deletePrefix(a, "dir/");                            // ...and the move's delete

    const auto inA = client->listObjects(a, "");
    EXPECT_TRUE(inA.prefixes.empty());
    ASSERT_EQ(inA.objects.size(), 2u);
    EXPECT_EQ(inA.objects[0].key, "one copy.txt");
    EXPECT_EQ(client->listObjects(b, "moved/sub/").objects.at(0).key, "moved/sub/y.txt");
    EXPECT_EQ(client->listObjects(b, "moved/").objects.at(0).size, 7u);

    client->deletePrefix(a, "");
    client->deletePrefix(b, "");
    client->deleteBucket(a);
    client->deleteBucket(b);
    std::filesystem::remove(src);
}

TEST(S3Test, BadKeysFailToConnect) {
    auto info = s3InfoFromEnv();
    if (!info)
        GTEST_SKIP() << "set DEARSQL_TEST_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY";
    info->password = "wrong";
    S3Database db(*info);
    EXPECT_FALSE(db.connect().first);
    EXPECT_FALSE(db.getLastConnectionError().empty());
}

// the agent's S3 tools, as `dearsql --mcp --allow-writes` serves them
TEST(S3Test, McpTools) {
    const auto info = s3InfoFromEnv();
    if (!info)
        GTEST_SKIP() << "set DEARSQL_TEST_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY";
    struct Host : mcp::Host {
        std::shared_ptr<const S3Client> client;
        std::vector<mcp::ConnectionEntry> connections() override {
            return {{"store", "s3", true}};
        }
        std::string connect(const std::string&) override {
            return "";
        }
        std::shared_ptr<dearsql::IConnection> connection(const std::string&) override {
            return nullptr;
        }
        std::shared_ptr<const S3Client> s3(const std::string& name) override {
            return name == "store" ? client : nullptr;
        }
    } host;
    host.client = std::make_shared<const S3Client>(S3Database::configFor(*info));
    mcp::Server server(host, true);
    auto call = [&](const char* tool, nlohmann::json args) {
        auto r = server.callTool(tool, args);
        EXPECT_FALSE(r["isError"].get<bool>()) << tool << ": " << r.dump();
        return r["content"][0]["text"].get<std::string>();
    };

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000;
    const std::string bucket = std::format("dearsql-mcp-{}", stamp);
    host.client->createBucket(bucket);
    const auto src = std::filesystem::temp_directory_path() / std::format("dearsql_mcp_{}", stamp);
    std::ofstream(src, std::ios::binary) << "id,name\n1,ada\n";

    call("upload_object", {{"path", src.string()}, {"bucket", bucket}, {"key", "in/people.csv"}});
    EXPECT_NE(call("list_objects", {}).find(bucket), std::string::npos); // the only S3 connection
    EXPECT_EQ(call("list_objects", {{"bucket", bucket}}), "in/\n");
    EXPECT_EQ(call("read_object", {{"bucket", bucket}, {"key", "in/people.csv"}}), "id,name\n1,ada\n");
    EXPECT_EQ(call("read_object", {{"bucket", bucket}, {"key", "in/people.csv"}, {"maxBytes", 2}}),
              "id\n[stopped at 2 bytes]");
    call("copy_object", {{"bucket", bucket}, {"key", "in/"}, {"toKey", "out/"}, {"move", true}});
    EXPECT_EQ(call("list_objects", {{"bucket", bucket}}), "out/\n");
    call("delete_object", {{"bucket", bucket}, {"key", "out/"}});
    EXPECT_TRUE(server.callTool("read_object", {{"bucket", bucket}, {"key", "nope"}})["isError"]);

    host.client->deletePrefix(bucket, "");
    host.client->deleteBucket(bucket);
    std::filesystem::remove(src);
}
