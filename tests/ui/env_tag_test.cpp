#include "ui/env_tag.hpp"

#include <gtest/gtest.h>

TEST(EnvTagGroupKey, LowercasesAsciiTag) {
    EXPECT_EQ(env_tag::groupKey("Prod"), "prod");
    EXPECT_EQ(env_tag::groupKey("PROD"), "prod");
    EXPECT_EQ(env_tag::groupKey("prod"), "prod");
}

TEST(EnvTagGroupKey, TrimsSurroundingWhitespace) {
    EXPECT_EQ(env_tag::groupKey(" prod"), "prod");
    EXPECT_EQ(env_tag::groupKey("prod "), "prod");
    EXPECT_EQ(env_tag::groupKey("  prod  "), "prod");
    EXPECT_EQ(env_tag::groupKey("\tprod\n"), "prod");
}

TEST(EnvTagGroupKey, TrimsAndLowercasesTogether) {
    EXPECT_EQ(env_tag::groupKey(" Prod "), "prod");
    EXPECT_EQ(env_tag::groupKey("  STAGING\t"), "staging");
}

TEST(EnvTagGroupKey, PreservesInternalWhitespace) {
    // Trim is edge-only, so a two-word tag stays two words.
    EXPECT_EQ(env_tag::groupKey("Prod Read Only"), "prod read only");
}

TEST(EnvTagGroupKey, EmptyStringMapsToEmpty) {
    EXPECT_EQ(env_tag::groupKey(""), "");
    EXPECT_EQ(env_tag::groupKey("   "), "");
    EXPECT_EQ(env_tag::groupKey("\t\n"), "");
}

TEST(EnvTagGroupKey, IdempotentOnAlreadyNormalisedInput) {
    EXPECT_EQ(env_tag::groupKey("prod"), "prod");
    EXPECT_EQ(env_tag::groupKey(env_tag::groupKey("Prod ")), "prod");
}

TEST(EnvTagGroupKey, KeepsAsciiDigitsAndDashes) {
    EXPECT_EQ(env_tag::groupKey("Us-East-1"), "us-east-1");
    EXPECT_EQ(env_tag::groupKey("prod-2"), "prod-2");
}
