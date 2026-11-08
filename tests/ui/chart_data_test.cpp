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

TEST(ChartData, TicksAlwaysEnd) {
    const auto a = chart::niceAxis(0, 9400, 5);
    EXPECT_EQ(a.lo, 0);
    EXPECT_EQ(a.hi, 10000);
    EXPECT_EQ(chart::ticks(a).size(), 6u); // 0, 2K .. 10K
    // a step below one ulp of the values: summing it would never move
    const auto big = chart::ticks(chart::niceAxis(1e17, 1e17 + 16, 5));
    EXPECT_FALSE(big.empty());
    EXPECT_LE(big.size(), 51u);
    EXPECT_LE(chart::ticks(chart::niceAxis(-1e308, 1e308, 5)).size(), 51u);
    EXPECT_EQ(chart::ticks(chart::niceAxis(7, 7, 5)).size() > 1, true); // flat data
}

TEST(ChartData, LongLabelsAreCappedOnACharacter) {
    std::string long_label;
    for (int i = 0; i < 5000; ++i)
        long_label += "é"; // two bytes each
    chart::Settings s;
    s.x = 0;
    s.ys = {1};
    const auto d = chart::build({"t", "v"}, {{long_label, "1"}}, s);
    ASSERT_EQ(d.labels.size(), 1u);
    EXPECT_EQ(d.labels[0], long_label.substr(0, chart::kMaxLabel * 2) + "...");
}

TEST(ChartData, NumericXIsSortedWithoutGrouping) {
    chart::Settings s;
    s.x = 0;
    s.ys = {1};
    const auto d = chart::build({"n", "v"}, {{"3", "30"}, {NUL, "0"}, {"1", "10"}, {"2", "20"}}, s);
    EXPECT_EQ(d.xs[0], 1);
    EXPECT_EQ(d.xs[1], 2);
    EXPECT_EQ(d.xs[2], 3);
    EXPECT_TRUE(std::isnan(d.xs[3])); // NULL x last
    EXPECT_EQ(d.series[0].values, (std::vector<double>{10, 20, 30, 0}));
}
