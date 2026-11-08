#include "utils/chart_data.hpp"

#include <algorithm>
#include <cctype>
#include <locale>
#include <sstream>
#include <cmath>
#include <dearsql/types.hpp>
#include <limits>
#include <unordered_map>

namespace chart {

    namespace {
        constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

        bool isNull(std::string_view cell) {
            return cell == dearsql::NULL_SENTINEL;
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        // "id", "user_id", "userId": numbers, but nothing worth plotting
        bool looksLikeId(const std::string& name) {
            std::string lower(name.size(), '\0');
            std::ranges::transform(name, lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
            return lower == "id" || lower.ends_with("_id") ||
                   (name.size() > 2 && name.ends_with("Id"));
        }

        struct Acc {
            double sum = 0, min = kNaN, max = kNaN;
            long long n = 0;
            void add(double v) {
                sum += v;
                min = n == 0 ? v : std::min(min, v);
                max = n == 0 ? v : std::max(max, v);
                ++n;
            }
            double get(Agg agg) const {
                switch (agg) {
                case Agg::Count:
                    return static_cast<double>(n);
                case Agg::Avg:
                    return n ? sum / static_cast<double>(n) : kNaN;
                case Agg::Min:
                    return min;
                case Agg::Max:
                    return max;
                default:
                    return n ? sum : kNaN;
                }
            }
        };
    } // namespace

    std::optional<double> parseNumber(std::string_view cell) {
        cell = trim(cell);
        if (cell.empty() || isNull(cell))
            return std::nullopt;
        if (cell.front() == '+')
            cell.remove_prefix(1);
        // ponytail: a C-locale stream; floating from_chars needs macOS 26, the app targets 14
        std::istringstream in{std::string(cell)};
        in.imbue(std::locale::classic());
        double v = 0;
        if (!(in >> v) || !(in >> std::ws).eof() || !std::isfinite(v))
            return std::nullopt;
        return v;
    }

    std::vector<bool> numericColumns(const std::vector<std::string>& columns,
                                     const std::vector<std::vector<std::string>>& rows) {
        std::vector<bool> numeric(columns.size(), true);
        std::vector<bool> seen(columns.size(), false);
        for (const auto& row : rows) {
            for (size_t c = 0; c < columns.size() && c < row.size(); ++c) {
                if (!numeric[c] || isNull(row[c]))
                    continue;
                if (parseNumber(row[c]))
                    seen[c] = true;
                else
                    numeric[c] = false;
            }
        }
        for (size_t c = 0; c < columns.size(); ++c)
            numeric[c] = numeric[c] && seen[c];
        return numeric;
    }

    Settings autoSettings(const std::vector<std::string>& columns,
                          const std::vector<std::vector<std::string>>& rows) {
        Settings s;
        const auto numeric = numericColumns(columns, rows);
        for (size_t c = 0; c < columns.size(); ++c) {
            if (!numeric[c]) {
                s.x = static_cast<int>(c);
                break;
            }
        }
        // all numbers: the first column is the x axis
        if (s.x < 0 && columns.size() > 1)
            s.x = 0;
        for (size_t c = 0; c < columns.size() && s.ys.size() < 4; ++c) {
            if (numeric[c] && static_cast<int>(c) != s.x && !looksLikeId(columns[c]))
                s.ys.push_back(static_cast<int>(c));
        }
        // only ids: plot them anyway rather than nothing
        for (size_t c = 0; s.ys.empty() && c < columns.size(); ++c) {
            if (numeric[c] && static_cast<int>(c) != s.x)
                s.ys.push_back(static_cast<int>(c));
        }
        const bool numericX = s.x >= 0 && numeric[s.x];
        s.kind = numericX ? Kind::Line : Kind::Bar;
        // repeated x values: one point per value
        if (s.x >= 0) {
            std::unordered_map<std::string, int> counts;
            for (const auto& row : rows) {
                if (static_cast<size_t>(s.x) < row.size() && ++counts[row[s.x]] > 1) {
                    s.agg = Agg::Sum;
                    break;
                }
            }
        }
        return s;
    }

    Data build(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows, const Settings& settings) {
        Data d;
        const int ncols = static_cast<int>(columns.size());
        const int x = settings.x < ncols ? settings.x : -1;
        std::vector<int> ys;
        for (int y : settings.ys) {
            if (y >= 0 && y < ncols)
                ys.push_back(y);
        }
        const auto cell = [](const std::vector<std::string>& row, int c) -> std::string_view {
            return c >= 0 && static_cast<size_t>(c) < row.size() ? std::string_view(row[c])
                                                                 : std::string_view{};
        };

        d.numericX = x >= 0 && numericColumns(columns, rows)[x];
        for (int y : ys)
            d.series.push_back({columns[y], {}});
        if (settings.agg == Agg::Count && ys.empty())
            d.series.push_back({"count", {}});

        const auto xLabel = [&](const std::vector<std::string>& row, size_t i) {
            if (x < 0)
                return std::to_string(i + 1);
            const auto v = cell(row, x);
            return isNull(v) ? std::string("NULL") : std::string(v);
        };

        if (settings.agg == Agg::None) {
            for (size_t i = 0; i < rows.size(); ++i) {
                d.labels.push_back(xLabel(rows[i], i));
                d.xs.push_back(d.numericX ? parseNumber(cell(rows[i], x)).value_or(kNaN)
                                          : static_cast<double>(i));
                for (size_t s = 0; s < ys.size(); ++s)
                    d.series[s].values.push_back(parseNumber(cell(rows[i], ys[s])).value_or(kNaN));
            }
            return d;
        }

        // grouped: first-seen order, or ascending when x is a number
        std::vector<std::string> order;
        std::unordered_map<std::string, std::vector<Acc>> groups;
        const size_t nseries = d.series.size();
        for (size_t i = 0; i < rows.size(); ++i) {
            std::string key = xLabel(rows[i], i);
            auto [it, inserted] = groups.try_emplace(key, nseries);
            if (inserted)
                order.push_back(key);
            for (size_t s = 0; s < ys.size(); ++s) {
                const auto v = parseNumber(cell(rows[i], ys[s]));
                if (v)
                    it->second[s].add(*v);
                else if (settings.agg == Agg::Count && !isNull(cell(rows[i], ys[s])))
                    it->second[s].add(0); // counts non-null text too
            }
            if (ys.empty() && nseries > 0)
                it->second[0].add(0); // count rows
        }
        if (d.numericX) {
            std::ranges::stable_sort(order, [](const std::string& a, const std::string& b) {
                return parseNumber(a).value_or(kNaN) < parseNumber(b).value_or(kNaN);
            });
        }
        for (const auto& key : order) {
            d.labels.push_back(key);
            d.xs.push_back(d.numericX ? parseNumber(key).value_or(kNaN)
                                      : static_cast<double>(d.labels.size() - 1));
            const auto& accs = groups[key];
            for (size_t s = 0; s < nseries; ++s)
                d.series[s].values.push_back(accs[s].get(settings.agg));
        }
        return d;
    }

    const char* kindName(Kind kind) {
        switch (kind) {
        case Kind::Bar:
            return "Bar";
        case Kind::Line:
            return "Line";
        case Kind::Area:
            return "Area";
        case Kind::Scatter:
            return "Scatter";
        case Kind::Pie:
            return "Pie";
        }
        return "";
    }

    const char* aggName(Agg agg) {
        switch (agg) {
        case Agg::None:
            return "None";
        case Agg::Sum:
            return "Sum";
        case Agg::Avg:
            return "Average";
        case Agg::Count:
            return "Count";
        case Agg::Min:
            return "Min";
        case Agg::Max:
            return "Max";
        }
        return "";
    }

} // namespace chart
