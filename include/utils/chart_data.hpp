#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// turns result rows (strings with dearsql sentinels) into chart series. no imgui
namespace chart {

    enum class Kind { Bar, Line, Area, Scatter, Pie };
    // None plots every row; the rest group rows by their x value
    enum class Agg { None, Sum, Avg, Count, Min, Max };

    struct Settings {
        Kind kind = Kind::Bar;
        int x = -1;          // column for the x axis; -1 = row number
        std::vector<int> ys; // columns plotted as series
        Agg agg = Agg::None;
    };

    struct Series {
        std::string name;
        std::vector<double> values; // one per point; NaN = no value
    };

    struct Data {
        std::vector<std::string> labels; // one per point (the x value as text)
        std::vector<double> xs;          // the x value as a number when numericX
        bool numericX = false;
        std::vector<Series> series;
    };

    // a plain decimal number, locale-free; NULL, booleans and text give nullopt
    std::optional<double> parseNumber(std::string_view cell);

    // columns whose non-null cells are all numbers (and that have at least one)
    std::vector<bool> numericColumns(const std::vector<std::string>& columns,
                                     const std::vector<std::vector<std::string>>& rows);

    // a reasonable first chart: x = first text column, y = the numeric ones (ids skipped)
    Settings autoSettings(const std::vector<std::string>& columns,
                          const std::vector<std::vector<std::string>>& rows);

    // out-of-range columns in settings are ignored
    Data build(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows, const Settings& settings);

    const char* kindName(Kind kind);
    const char* aggName(Agg agg);

} // namespace chart
