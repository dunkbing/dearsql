#include "database/s3.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>

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

TEST(S3Test, BadKeysFailToConnect) {
    auto info = s3InfoFromEnv();
    if (!info)
        GTEST_SKIP() << "set DEARSQL_TEST_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY";
    info->password = "wrong";
    S3Database db(*info);
    EXPECT_FALSE(db.connect().first);
    EXPECT_FALSE(db.getLastConnectionError().empty());
}
