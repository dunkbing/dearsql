#include "utils/cli_command.hpp"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

TEST(CliCommand, InstallsAndRepointsLink) {
    if (!CliCommand::supported())
        GTEST_SKIP();
    const auto dir = std::filesystem::temp_directory_path() / "dearsql_cli_test" / "bin";
    std::filesystem::remove_all(dir.parent_path());
    const auto link = dir / "dearsql";
    setenv("DEARSQL_CLI_LINK", link.c_str(), 1);

    EXPECT_FALSE(CliCommand::installed());
    ASSERT_EQ(CliCommand::install(), "");
    EXPECT_TRUE(CliCommand::installed());

    // a stale link to something else is replaced
    std::filesystem::remove(link);
    std::filesystem::create_symlink("/bin/sh", link);
    EXPECT_FALSE(CliCommand::installed());
    ASSERT_EQ(CliCommand::install(), "");
    EXPECT_TRUE(CliCommand::installed());

    unsetenv("DEARSQL_CLI_LINK");
    std::filesystem::remove_all(dir.parent_path());
}
