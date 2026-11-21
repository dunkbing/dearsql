// saved theme ids and connection colour keys still resolve
#include "themes.hpp"

#include <gtest/gtest.h>

TEST(Themes, SavedIds) {
    EXPECT_STREQ(themes::byId("light").id, "one-light");
    EXPECT_STREQ(themes::byId("dark").id, "one-dark");
    EXPECT_STREQ(themes::byId("github-dark").id, "github-dark");
    EXPECT_STREQ(themes::byId("no-such-theme").id, imshell::Themes()[0].id);
    // the os look where imshell reads one (macos), the first preset elsewhere
    EXPECT_STREQ(themes::byId("system").id, imshell::SystemThemeAvailable() ? "system" : imshell::Themes()[0].id);
}

TEST(Themes, ConnectionColorKeys) {
    for (const char* key : {"red", "peach", "yellow", "green", "blue", "purple"})
        EXPECT_TRUE(themes::connectionColor(key).has_value()) << key;
    EXPECT_FALSE(themes::connectionColor("").has_value());
    EXPECT_FALSE(themes::connectionColor("teal").has_value());
}
