#include "utils/chart_data.hpp"

#include <cmath>
#include <dearsql/types.hpp>
#include <gtest/gtest.h>

namespace {
    const std::string NUL(dearsql::NULL_SENTINEL);
}

TEST(ChartData, ParseNumber) {
    EXPECT_EQ(chart::parseNumber("42"), 42.0);
    EXPECT_EQ(chart::parseNumber(" -2.5 "), -2.5);
    EXPECT_EQ(chart::parseNumber("+3"), 3.0);
    EXPECT_EQ(chart::parseNumber("1e3"), 1000.0);
    EXPECT_FALSE(chart::parseNumber("12abc"));
    EXPECT_FALSE(chart::parseNumber(""));
    EXPECT_FALSE(chart::parseNumber(NUL));
    EXPECT_FALSE(chart::parseNumber("inf"));
    EXPECT_FALSE(chart::parseNumber(std::string(dearsql::BOOL_TRUE_SENTINEL)));
}

TEST(ChartData, AutoPicksTextXAndNumericYs) {
    const std::vector<std::string> cols{"genre_id", "genre", "tracks", "minutes"};
    const std::vector<std::vector<std::string>> rows{
        {"1", "Rock", "1297", NUL}, {"2", "Jazz", "130", "40.5"}, {"3", "Metal", "374", "99"}};
    const auto s = chart::autoSettings(cols, rows);
    EXPECT_EQ(s.x, 1);
    EXPECT_EQ(s.ys, (std::vector<int>{2, 3})); // the id is skipped
    EXPECT_EQ(s.kind, chart::Kind::Bar);
    EXPECT_EQ(s.agg, chart::Agg::None); // every genre once

    const auto d = chart::build(cols, rows, s);
    EXPECT_EQ(d.labels, (std::vector<std::string>{"Rock", "Jazz", "Metal"}));
    ASSERT_EQ(d.series.size(), 2u);
    EXPECT_EQ(d.series[0].values[0], 1297.0);
    EXPECT_TRUE(std::isnan(d.series[1].values[0])); // NULL is a gap
}

TEST(ChartData, AllNumbersMakeALine) {
    const auto s = chart::autoSettings({"year", "total"}, {{"2021", "10"}, {"2022", "12"}});
    EXPECT_EQ(s.x, 0);
    EXPECT_EQ(s.ys, (std::vector<int>{1}));
    EXPECT_EQ(s.kind, chart::Kind::Line);
}

TEST(ChartData, GroupsRepeatedX) {
    const std::vector<std::string> cols{"country", "total"};
    const std::vector<std::vector<std::string>> rows{
        {"US", "10"}, {"FR", "5"}, {"US", "2.5"}, {"FR", NUL}, {NUL, "1"}};
    auto s = chart::autoSettings(cols, rows);
    EXPECT_EQ(s.agg, chart::Agg::Sum); // US twice
    auto d = chart::build(cols, rows, s);
    EXPECT_EQ(d.labels, (std::vector<std::string>{"US", "FR", "NULL"}));
    EXPECT_EQ(d.series[0].values, (std::vector<double>{12.5, 5, 1}));

    s.agg = chart::Agg::Count;
    d = chart::build(cols, rows, s);
    EXPECT_EQ(d.series[0].values, (std::vector<double>{2, 1, 1})); // NULL total not counted
    s.agg = chart::Agg::Avg;
    EXPECT_EQ(chart::build(cols, rows, s).series[0].values[0], 6.25);
    s.agg = chart::Agg::Max;
    EXPECT_EQ(chart::build(cols, rows, s).series[0].values[0], 10.0);
}

TEST(ChartData, NumericXGroupsSortAscending) {
    chart::Settings s;
    s.x = 0;
    s.ys = {1};
    s.agg = chart::Agg::Sum;
    const auto d = chart::build({"n", "v"}, {{"10", "1"}, {"2", "1"}, {"10", "1"}}, s);
    EXPECT_TRUE(d.numericX);
    EXPECT_EQ(d.labels, (std::vector<std::string>{"2", "10"}));
    EXPECT_EQ(d.xs, (std::vector<double>{2, 10}));
}

TEST(ChartData, StaleColumnsAreIgnored) {
    // settings picked for a wider result, then a one-column result arrives
    chart::Settings s;
    s.x = 2;
    s.ys = {1, 5};
    s.agg = chart::Agg::Sum;
    const auto d = chart::build({"a"}, {{"1"}, {"2"}}, s);
    EXPECT_TRUE(d.series.empty());
    EXPECT_EQ(d.labels.size(), 2u); // x fell back to the row number
    s.ys.clear();
    s.agg = chart::Agg::Avg;
    EXPECT_NO_THROW(chart::build({"a"}, {{"1"}}, s)); // no y, not a count: nothing to plot
}
