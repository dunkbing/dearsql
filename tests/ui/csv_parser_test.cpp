#include "utils/csv_parser.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace {

    std::filesystem::path makeTempPath() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               std::format("dearsql_csv_parser_test_{}.csv", stamp);
    }

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

} // namespace

TEST(CsvParserTest, ParseTextAcceptsEmptyInput) {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;

    EXPECT_TRUE(CsvParser::parseText("", headers, rows));
    EXPECT_TRUE(headers.empty());
    EXPECT_TRUE(rows.empty());
}

TEST(CsvParserTest, SerializeEmptyModelProducesEmptyString) {
    EXPECT_TRUE(CsvParser::serialize({}, {}).empty());
}

TEST(CsvParserTest, WriteFileHandlesEmptyModel) {
    const auto path = makeTempPath();

    ASSERT_TRUE(CsvParser::writeFile(path.string(), {}, {}));
    EXPECT_TRUE(readFile(path).empty());

    std::filesystem::remove(path);
}

TEST(CsvParserTest, WriteFileHandlesRowsWithoutHeaders) {
    const auto path = makeTempPath();
    const std::vector<std::vector<std::string>> rows = {{"alice", "admin"}, {"bob", "user"}};

    ASSERT_TRUE(CsvParser::writeFile(path.string(), {}, rows));
    EXPECT_EQ(readFile(path), "alice,admin\nbob,user\n");

    std::filesystem::remove(path);
}
