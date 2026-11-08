#include "ui/result_chart.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "themes.hpp"
#include "utils/menu.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace {
    constexpr chart::Kind kKinds[] = {chart::Kind::Bar, chart::Kind::Line, chart::Kind::Area,
                                      chart::Kind::Scatter, chart::Kind::Pie};
    constexpr chart::Agg kAggs[] = {chart::Agg::None,  chart::Agg::Sum, chart::Agg::Avg,
                                    chart::Agg::Count, chart::Agg::Min, chart::Agg::Max};

    ImU32 seriesColor(size_t i) {
        const auto& c = Application::getInstance().getCurrentColors();
        const ImVec4 palette[] = {c.blue, c.peach,  c.green,   c.purple, c.teal,
                                  c.red,  c.yellow, c.magenta, c.maroon, c.lavender};
        return ImGui::GetColorU32(palette[i % std::size(palette)]);
    }

    ImU32 withAlpha(ImU32 color, float alpha) {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(color);
        v.w = alpha;
        return ImGui::GetColorU32(v);
    }

    // 1, 2 or 5 times a power of ten, about target steps across the range
    double niceStep(double range, int target) {
        const double raw = range / target;
        const double mag = std::pow(10.0, std::floor(std::log10(raw)));
        const double f = raw / mag;
        return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
    }

    struct Axis {
        double lo = 0, hi = 1, step = 1;
    };

    Axis niceAxis(double lo, double hi, int target) {
        if (!(hi > lo)) {
            const double pad = lo == 0 ? 1 : std::abs(lo) * 0.5;
            lo -= pad;
            hi += pad;
        }
        Axis a;
        a.step = niceStep(hi - lo, target);
        a.lo = std::floor(lo / a.step) * a.step;
        a.hi = std::ceil(hi / a.step) * a.step;
        return a;
    }

    // tick labels share one unit, picked from the largest tick: 9K, 10K rather than 9000, 10K
    struct TickFormat {
        double div = 1;
        const char* suffix = "";
    };
    TickFormat tickFormat(const Axis& a) {
        const double m = std::max(std::abs(a.lo), std::abs(a.hi));
        if (m >= 1e9)
            return {1e9, "B"};
        if (m >= 1e6)
            return {1e6, "M"};
        if (m >= 1e4)
            return {1e3, "K"};
        return {};
    }
    std::string formatTick(double v, const TickFormat& f) {
        if (std::abs(v) < 1e-12)
            v = 0; // no "-0" or 1e-17 from summed steps
        return std::format("{:.6g}{}", v / f.div, v == 0 ? "" : f.suffix);
    }

    // a label cut to fit width, with an ellipsis
    std::string fit(const std::string& text, float width) {
        if (ImGui::CalcTextSize(text.c_str()).x <= width)
            return text;
        std::string s = text;
        while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width)
            s.pop_back();
        return s.empty() ? "" : s + "...";
    }

    void tooltip(const chart::Data& d, size_t i, size_t onlySeries = SIZE_MAX) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(d.labels[i].c_str());
        for (size_t s = 0; s < d.series.size(); ++s) {
            if (onlySeries != SIZE_MAX && s != onlySeries)
                continue;
            const double v = d.series[s].values[i];
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(seriesColor(s)), ICON_FA_SQUARE);
            ImGui::SameLine();
            ImGui::Text("%s: %s", d.series[s].name.c_str(),
                        std::isfinite(v) ? std::format("{:.10g}", v).c_str() : "NULL");
        }
        ImGui::EndTooltip();
    }
} // namespace

void ResultChart::render(const char* id, const std::vector<std::string>& columns,
                         const std::vector<std::vector<std::string>>& rows) {
    lastFrame_ = ImGui::GetFrameCount();
    // new columns: start over; new rows: same choices, fresh numbers
    if (columns != columns_) {
        columns_ = columns;
        settings_ = chart::autoSettings(columns, rows);
        rows_ = nullptr;
    }
    if (rows_ != rows.data() || size_ != rows.size()) {
        rows_ = rows.data();
        size_ = rows.size();
        numeric_ = chart::numericColumns(columns, rows);
        dirty_ = true;
    }
    if (dirty_) {
        data_ = chart::build(columns, rows, settings_);
        dirty_ = false;
    }

    ImGui::PushID(id);
    renderControls();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::XS));
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (data_.series.empty() || data_.labels.empty()) {
        ImGui::TextDisabled(data_.labels.empty() ? "No rows to chart."
                                                 : "Pick a numeric column for Y.");
    } else if (settings_.kind == chart::Kind::Pie) {
        renderPie(size);
    } else {
        renderCartesian(size);
    }
    ImGui::PopID();
    if (dirty_) { // a control changed
        data_ = chart::build(columns, rows, settings_);
        dirty_ = false;
    }
}

void ResultChart::renderControls() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::Spacing::M, Theme::Spacing::S));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    const auto label = [](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
        ImGui::SameLine(0, Theme::Spacing::S);
    };

    ImGui::SetNextItemWidth(110.0f);
    if (UIUtils::BeginSelect("##chart_kind", chart::kindName(settings_.kind))) {
        for (auto kind : kKinds) {
            if (ImGui::Selectable(chart::kindName(kind), kind == settings_.kind)) {
                settings_.kind = kind;
                dirty_ = true;
            }
        }
        UIUtils::EndSelect();
    }

    ImGui::SameLine(0, Theme::Spacing::L);
    label("X");
    const std::string xName = settings_.x >= 0 && settings_.x < static_cast<int>(columns_.size())
                                  ? columns_[settings_.x]
                                  : "Row number";
    ImGui::SetNextItemWidth(160.0f);
    if (UIUtils::BeginSelect("##chart_x", xName.c_str())) {
        if (ImGui::Selectable("Row number", settings_.x < 0)) {
            settings_.x = -1;
            dirty_ = true;
        }
        for (int c = 0; c < static_cast<int>(columns_.size()); ++c) {
            if (ImGui::Selectable(std::format("{}##x{}", columns_[c], c).c_str(),
                                  c == settings_.x)) {
                settings_.x = c;
                dirty_ = true;
            }
        }
        UIUtils::EndSelect();
    }

    ImGui::SameLine(0, Theme::Spacing::L);
    label("Y");
    std::string yNames;
    for (int y : settings_.ys) {
        if (y < static_cast<int>(columns_.size()))
            yNames += (yNames.empty() ? "" : ", ") + columns_[y];
    }
    ImGui::SetNextItemWidth(200.0f);
    if (UIUtils::BeginSelect("##chart_y", yNames.empty() ? "None" : yNames.c_str())) {
        bool any = false;
        for (int c = 0; c < static_cast<int>(columns_.size()); ++c) {
            if (c >= static_cast<int>(numeric_.size()) || !numeric_[c])
                continue;
            any = true;
            bool on = std::ranges::find(settings_.ys, c) != settings_.ys.end();
            if (ImGui::Checkbox(std::format("{}##y{}", columns_[c], c).c_str(), &on)) {
                if (on)
                    settings_.ys.push_back(c);
                else
                    std::erase(settings_.ys, c);
                std::ranges::sort(settings_.ys);
                dirty_ = true;
            }
        }
        if (!any)
            ImGui::TextDisabled("No numeric columns");
        UIUtils::EndSelect();
    }

    ImGui::SameLine(0, Theme::Spacing::L);
    label("Group by X");
    ImGui::SetNextItemWidth(110.0f);
    if (UIUtils::BeginSelect("##chart_agg", chart::aggName(settings_.agg))) {
        for (auto agg : kAggs) {
            if (ImGui::Selectable(chart::aggName(agg), agg == settings_.agg)) {
                settings_.agg = agg;
                dirty_ = true;
            }
        }
        UIUtils::EndSelect();
    }
    ImGui::PopStyleVar(2);
}

void ResultChart::renderCartesian(const ImVec2& size) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const chart::Data& d = data_;
    const size_t n = d.labels.size();
    const bool bars = settings_.kind == chart::Kind::Bar;
    const bool numericX = d.numericX && !bars;

    double lo = INFINITY, hi = -INFINITY;
    for (const auto& s : d.series) {
        for (double v : s.values) {
            if (std::isfinite(v))
                lo = std::min(lo, v), hi = std::max(hi, v);
        }
    }
    if (!std::isfinite(lo)) {
        ImGui::TextDisabled("No numbers to plot in the Y columns.");
        return;
    }
    if (bars || settings_.kind == chart::Kind::Area)
        lo = std::min(lo, 0.0), hi = std::max(hi, 0.0);
    const Axis y = niceAxis(lo, hi, std::max(2, static_cast<int>(size.y / 60)));
    const TickFormat yFmt = tickFormat(y);

    double xlo = 0, xhi = 1;
    Axis xAxis;
    TickFormat xFmt;
    if (numericX) {
        xlo = INFINITY, xhi = -INFINITY;
        for (double x : d.xs) {
            if (std::isfinite(x))
                xlo = std::min(xlo, x), xhi = std::max(xhi, x);
        }
        if (!std::isfinite(xlo))
            xlo = 0, xhi = 1;
        xAxis = niceAxis(xlo, xhi, std::max(2, static_cast<int>(size.x / 120)));
        xFmt = tickFormat(xAxis);
        xlo = xAxis.lo, xhi = xAxis.hi;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##plot", ImVec2(std::max(size.x, 50.0f), std::max(size.y, 50.0f)));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();

    // legend across the top
    float lx = origin.x;
    for (size_t s = 0; s < d.series.size(); ++s) {
        dl->AddRectFilled(ImVec2(lx, origin.y + lineH * 0.25f),
                          ImVec2(lx + lineH * 0.5f, origin.y + lineH * 0.75f), seriesColor(s));
        lx += lineH * 0.5f + Theme::Spacing::S;
        dl->AddText(ImVec2(lx, origin.y), ImGui::GetColorU32(colors.subtext0),
                    d.series[s].name.c_str());
        lx += ImGui::CalcTextSize(d.series[s].name.c_str()).x + Theme::Spacing::L;
    }

    float yLabelW = 0;
    for (double v = y.lo; v <= y.hi + y.step * 0.5; v += y.step)
        yLabelW = std::max(yLabelW, ImGui::CalcTextSize(formatTick(v, yFmt).c_str()).x);
    const float left = origin.x + yLabelW + Theme::Spacing::M;
    const float right = origin.x + size.x - Theme::Spacing::M;
    const float top = origin.y + lineH + Theme::Spacing::M;
    const float bottom = origin.y + size.y - lineH - Theme::Spacing::M;
    if (right - left < 20 || bottom - top < 20)
        return;
    const auto py = [&](double v) {
        return bottom - static_cast<float>((v - y.lo) / (y.hi - y.lo)) * (bottom - top);
    };
    const float slot = (right - left) / static_cast<float>(n);
    const auto px = [&](size_t i) {
        if (numericX)
            return left + static_cast<float>((d.xs[i] - xlo) / (xhi - xlo)) * (right - left);
        return left + (static_cast<float>(i) + 0.5f) * slot;
    };

    // grid and y labels
    const ImU32 grid = ImGui::GetColorU32(colors.surface0);
    const ImU32 tickText = ImGui::GetColorU32(colors.subtext0);
    for (double v = y.lo; v <= y.hi + y.step * 0.5; v += y.step) {
        const float yy = py(v);
        dl->AddLine(ImVec2(left, yy), ImVec2(right, yy), grid);
        const std::string t = formatTick(v, yFmt);
        dl->AddText(
            ImVec2(left - Theme::Spacing::S - ImGui::CalcTextSize(t.c_str()).x, yy - lineH * 0.5f),
            tickText, t.c_str());
    }
    dl->AddLine(ImVec2(left, bottom), ImVec2(right, bottom), ImGui::GetColorU32(colors.overlay0));

    // x labels: numeric ticks, or every k-th category so they don't overlap
    if (numericX) {
        for (double v = xAxis.lo; v <= xAxis.hi + xAxis.step * 0.5; v += xAxis.step) {
            const float xx = left + static_cast<float>((v - xlo) / (xhi - xlo)) * (right - left);
            const std::string t = formatTick(v, xFmt);
            dl->AddText(
                ImVec2(xx - ImGui::CalcTextSize(t.c_str()).x * 0.5f, bottom + Theme::Spacing::XS),
                tickText, t.c_str());
        }
    } else {
        // widest label (capped), then every k-th one so neighbours never touch
        float labelW = 0;
        for (const auto& l : d.labels)
            labelW = std::max(labelW, ImGui::CalcTextSize(l.c_str()).x);
        labelW = std::min(labelW, 140.0f);
        const size_t every =
            std::max<size_t>(1, static_cast<size_t>(std::ceil((labelW + 8.0f) / slot)));
        for (size_t i = 0; i < n; i += every) {
            const std::string t = fit(d.labels[i], slot * static_cast<float>(every) - 8.0f);
            dl->AddText(ImVec2(px(i) - ImGui::CalcTextSize(t.c_str()).x * 0.5f,
                               bottom + Theme::Spacing::XS),
                        tickText, t.c_str());
        }
    }

    dl->PushClipRect(ImVec2(left, top - 1), ImVec2(right + 1, bottom + 1), true);
    const float base = py(std::clamp(0.0, y.lo, y.hi));
    const size_t ns = d.series.size();
    if (bars) {
        const float group = slot * 0.8f;
        const float barW = group / static_cast<float>(ns);
        for (size_t i = 0; i < n; ++i) {
            for (size_t s = 0; s < ns; ++s) {
                const double v = d.series[s].values[i];
                if (!std::isfinite(v))
                    continue;
                const float x0 = px(i) - group * 0.5f + barW * static_cast<float>(s);
                const float yv = py(v);
                dl->AddRectFilled(ImVec2(x0 + (barW > 4 ? 1.0f : 0.0f), std::min(yv, base)),
                                  ImVec2(x0 + barW, std::max(yv, base)), seriesColor(s), 2.0f,
                                  v >= 0 ? ImDrawFlags_RoundCornersTop
                                         : ImDrawFlags_RoundCornersBottom);
            }
        }
    } else {
        for (size_t s = 0; s < ns; ++s) {
            const ImU32 col = seriesColor(s);
            const auto& vals = d.series[s].values;
            const bool scatter = settings_.kind == chart::Kind::Scatter;
            for (size_t i = 0; i < n; ++i) {
                if (!std::isfinite(vals[i]) || (numericX && !std::isfinite(d.xs[i])))
                    continue;
                const ImVec2 p(px(i), py(vals[i]));
                if (scatter) {
                    dl->AddCircleFilled(p, 3.5f, withAlpha(col, 0.8f));
                    continue;
                }
                // segment to the next point; a NULL breaks the line
                if (i + 1 < n && std::isfinite(vals[i + 1]) &&
                    (!numericX || std::isfinite(d.xs[i + 1]))) {
                    const ImVec2 q(px(i + 1), py(vals[i + 1]));
                    if (settings_.kind == chart::Kind::Area) {
                        const ImVec2 quad[] = {p, q, ImVec2(q.x, base), ImVec2(p.x, base)};
                        dl->AddConvexPolyFilled(quad, 4, withAlpha(col, 0.25f));
                    }
                    dl->AddLine(p, q, col, 2.0f);
                }
                if (n <= 60)
                    dl->AddCircleFilled(p, 2.5f, col);
            }
        }
    }

    // hover: the nearest point, a guide line and its values
    if (hovered) {
        const float mx = ImGui::GetIO().MousePos.x;
        size_t best = 0;
        float bestDist = INFINITY;
        for (size_t i = 0; i < n; ++i) {
            if (numericX && !std::isfinite(d.xs[i]))
                continue;
            const float dist = std::abs(px(i) - mx);
            if (dist < bestDist)
                best = i, bestDist = dist;
        }
        dl->AddLine(ImVec2(px(best), top), ImVec2(px(best), bottom),
                    ImGui::GetColorU32(colors.overlay1));
        tooltip(d, best);
    }
    dl->PopClipRect();
}

void ResultChart::renderPie(const ImVec2& size) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const chart::Data& d = data_;
    // the first series; positive values only, biggest first, the rest folded into "Other"
    struct Slice {
        std::string label;
        double value;
    };
    std::vector<Slice> slices;
    for (size_t i = 0; i < d.labels.size(); ++i) {
        const double v = d.series[0].values[i];
        if (std::isfinite(v) && v > 0)
            slices.push_back({d.labels[i], v});
    }
    if (slices.empty()) {
        ImGui::TextDisabled("A pie needs positive values in %s.", d.series[0].name.c_str());
        return;
    }
    std::ranges::stable_sort(slices,
                             [](const Slice& a, const Slice& b) { return a.value > b.value; });
    constexpr size_t kMaxSlices = 10;
    if (slices.size() > kMaxSlices) {
        double other = 0;
        for (size_t i = kMaxSlices - 1; i < slices.size(); ++i)
            other += slices[i].value;
        slices.resize(kMaxSlices - 1);
        slices.push_back({"Other", other});
    }
    double total = 0;
    for (const auto& s : slices)
        total += s.value;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pie", ImVec2(std::max(size.x, 50.0f), std::max(size.y, 50.0f)));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();

    const float legendW = std::min(260.0f, size.x * 0.4f);
    const float radius =
        std::max(10.0f, std::min(size.x - legendW - Theme::Spacing::XL * 2, size.y) * 0.5f -
                            Theme::Spacing::M);
    const ImVec2 center(origin.x + Theme::Spacing::M + radius, origin.y + size.y * 0.5f);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mdx = mouse.x - center.x, mdy = mouse.y - center.y;
    const bool inPie = hovered && mdx * mdx + mdy * mdy <= radius * radius;
    float mouseAngle = std::atan2(mdy, mdx) + std::numbers::pi_v<float> * 0.5f;
    if (mouseAngle < 0)
        mouseAngle += std::numbers::pi_v<float> * 2;

    constexpr float tau = std::numbers::pi_v<float> * 2;
    float a0 = 0;
    int hoveredSlice = -1;
    for (size_t i = 0; i < slices.size(); ++i) {
        const float a1 = a0 + static_cast<float>(slices[i].value / total) * tau;
        if (inPie && mouseAngle >= a0 && mouseAngle < a1)
            hoveredSlice = static_cast<int>(i);
        const ImU32 col =
            hoveredSlice == static_cast<int>(i) ? withAlpha(seriesColor(i), 0.8f) : seriesColor(i);
        // wedges of at most ~30 degrees stay convex
        const int pieces = std::max(1, static_cast<int>(std::ceil((a1 - a0) / 0.5f)));
        for (int p = 0; p < pieces; ++p) {
            const float s0 = a0 + (a1 - a0) * static_cast<float>(p) / static_cast<float>(pieces);
            const float s1 =
                a0 + (a1 - a0) * static_cast<float>(p + 1) / static_cast<float>(pieces);
            // pieces overlap a little so their anti-aliased edges leave no seam
            const float s1x = p + 1 < pieces ? s1 + 0.02f : s1;
            dl->PathLineTo(center);
            dl->PathArcTo(center, radius, s0 - tau / 4, s1x - tau / 4);
            dl->PathFillConvex(col);
        }
        a0 = a1;
    }
    // thin gaps between slices
    a0 = 0;
    for (const auto& s : slices) {
        const float ang = a0 - tau / 4;
        dl->AddLine(center,
                    ImVec2(center.x + std::cos(ang) * radius, center.y + std::sin(ang) * radius),
                    ImGui::GetColorU32(colors.base), 1.5f);
        a0 += static_cast<float>(s.value / total) * tau;
    }

    // legend on the right, titled with the plotted column
    float ly =
        center.y - static_cast<float>(slices.size() + 1) * (lineH + Theme::Spacing::XS) * 0.5f;
    const float lx = center.x + radius + Theme::Spacing::XL;
    dl->AddText(ImVec2(lx, ly), ImGui::GetColorU32(colors.text), d.series[0].name.c_str());
    ly += lineH + Theme::Spacing::XS;
    for (size_t i = 0; i < slices.size(); ++i) {
        dl->AddRectFilled(ImVec2(lx, ly + lineH * 0.2f),
                          ImVec2(lx + lineH * 0.6f, ly + lineH * 0.8f), seriesColor(i), 2.0f);
        const std::string text =
            std::format("{}  {:.1f}%", fit(slices[i].label, legendW - lineH - 60.0f),
                        slices[i].value / total * 100.0);
        dl->AddText(
            ImVec2(lx + lineH, ly),
            ImGui::GetColorU32(hoveredSlice == static_cast<int>(i) ? colors.text : colors.subtext0),
            text.c_str());
        ly += lineH + Theme::Spacing::XS;
    }

    if (hoveredSlice >= 0) {
        const auto& s = slices[hoveredSlice];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(s.label.c_str());
        ImGui::Text("%s: %s (%.1f%%)", d.series[0].name.c_str(),
                    std::format("{:.10g}", s.value).c_str(), s.value / total * 100.0);
        ImGui::EndTooltip();
    }
}
