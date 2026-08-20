#include "ui/datetime_picker.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>

namespace {

    std::string toUpperStr(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s)
            r += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }

    bool isLeapYear(int y) {
        return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0);
    }

    int daysInMonth(int month, int year) {
        static const int d[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month == 2 && isLeapYear(year))
            return 29;
        return d[month];
    }

    // Zeller's: returns 0=Sun, 1=Mon, ..., 6=Sat
    int dayOfWeekSun0(int day, int month, int year) {
        if (month <= 2) {
            month += 12;
            year -= 1;
        }
        int h = (day + (13 * (month + 1)) / 5 + year + year / 4 - year / 100 + year / 400) % 7;
        return ((h + 6) % 7); // 0=Sun
    }

    void getNowTm(tm& out) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
#ifdef _MSC_VER
        localtime_s(&out, &t);
#else
        localtime_r(&t, &out);
#endif
    }

    std::string formatDate(const tm& t) {
        return std::format("{:04d}-{:02d}-{:02d}", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }

    std::string formatDateTime(const tm& t, int h, int m, int s) {
        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", t.tm_year + 1900,
                           t.tm_mon + 1, t.tm_mday, h, m, s);
    }

    constexpr float CELL_W = 32.0f;
    constexpr float SPINNER_W = 40.0f;
    constexpr float QUICK_W = 108.0f;
    constexpr float ITEM_GAP = Theme::Spacing::XS;

    bool paddedSelectable(const char* id, const char* label, const ImVec2& size) {
        bool clicked = ImGui::Selectable(id, false, 0, size);
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        ImVec2 textSize = ImGui::CalcTextSize(label);
        ImVec2 textPos(min.x + Theme::Spacing::M, min.y + (size.y - textSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), label);
        return clicked;
    }

} // namespace

namespace DateTimePicker {

    int columnKind(const std::string& columnType) {
        if (columnType.empty())
            return 0;
        std::string u = toUpperStr(columnType);
        if (u == "DATE")
            return 1;
        if (u.find("TIMESTAMP") != std::string::npos || u.find("DATETIME") != std::string::npos)
            return 2;
        return 0;
    }

    bool parse(const std::string& s, DateTimePickerState& state) {
        if (s.size() < 10)
            return false;
        int y = 0, m = 0, d = 0;
        if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
            return false;
        state.date = {};
        state.date.tm_year = y - 1900;
        state.date.tm_mon = m - 1;
        state.date.tm_mday = d;
        state.date.tm_isdst = -1;
        state.suffix.clear();
        state.hour = state.minute = state.second = 0;
        if (s.size() > 10) {
            int consumed = 0;
            if (std::sscanf(s.c_str() + 11, "%d:%d:%d%n", &state.hour, &state.minute, &state.second,
                            &consumed) == 3 &&
                consumed > 0) {
                state.suffix = s.substr(11 + consumed);
            }
        }
        return true;
    }

    std::string format(const DateTimePickerState& state) {
        if (state.kind == 1)
            return formatDate(state.date);
        return formatDateTime(state.date, state.hour, state.minute, state.second) + state.suffix;
    }

    DateTimePickerResult render(DateTimePickerState& state) {
        const auto& colors = Application::getInstance().getCurrentColors();
        const bool hasTime = (state.kind == 2);
        DateTimePickerResult result;

        // --- left panel: quick actions ---
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::S));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ITEM_GAP));

        auto quickBtn = [](const char* label) {
            return paddedSelectable(std::format("##quick_{}", label).c_str(), label,
                                    ImVec2(QUICK_W, ImGui::GetFrameHeight()));
        };

        if (state.allowNull && quickBtn("NULL")) {
            result.setNull = true;
            result.committed = true;
            ImGui::PopStyleVar(2);
            ImGui::EndGroup();
            return result;
        }
        if (quickBtn("now")) {
            getNowTm(state.date);
            state.hour = state.date.tm_hour;
            state.minute = state.date.tm_min;
            state.second = state.date.tm_sec;
            result.changed = true;
            result.committed = true;
            result.value = format(state);
            ImGui::PopStyleVar(2);
            ImGui::EndGroup();
            return result;
        }
        if (quickBtn("today")) {
            getNowTm(state.date);
            state.hour = state.minute = state.second = 0;
            result.changed = true;
        }
        if (quickBtn("tomorrow")) {
            getNowTm(state.date);
            state.date.tm_mday += 1;
            std::mktime(&state.date);
            state.hour = state.minute = state.second = 0;
            result.changed = true;
        }
        if (quickBtn("yesterday")) {
            getNowTm(state.date);
            state.date.tm_mday -= 1;
            std::mktime(&state.date);
            state.hour = state.minute = state.second = 0;
            result.changed = true;
        }

        ImGui::PopStyleVar(2);
        ImGui::EndGroup();
        ImGui::SameLine(0, Theme::Spacing::L);

        // --- center panel: calendar ---
        ImGui::BeginGroup();
        int month = state.date.tm_mon + 1;
        int year = state.date.tm_year + 1900;
        int selectedDay = state.date.tm_mday;

        // month/year navigation row
        constexpr float calWidth = 7 * CELL_W + 6 * ITEM_GAP;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::ArrowButton("##prev_m", ImGuiDir_Left)) {
            if (--month < 1) {
                month = 12;
                year--;
            }
            state.date.tm_mon = month - 1;
            state.date.tm_year = year - 1900;
            state.date.tm_mday = std::min(selectedDay, daysInMonth(month, year));
        }
        ImGui::SameLine();

        static const char* monthNames[] = {"January",   "February", "March",    "April",
                                           "May",       "June",     "July",     "August",
                                           "September", "October",  "November", "December"};
        std::string headerText = std::format("{} {}", monthNames[month - 1], year);
        float headerW = ImGui::CalcTextSize(headerText.c_str()).x;
        float arrowW = ImGui::GetFrameHeight();
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float pad = (calWidth - headerW - 2 * arrowW - 2 * spacing) * 0.5f;
        if (pad > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", headerText.c_str());
        ImGui::SameLine();
        if (pad > 0) {
            float rightArrowX = ImGui::GetCursorPosX();
            float targetX = rightArrowX + pad;
            ImGui::SetCursorPosX(targetX);
        }

        if (ImGui::ArrowButton("##next_m", ImGuiDir_Right)) {
            if (++month > 12) {
                month = 1;
                year++;
            }
            state.date.tm_mon = month - 1;
            state.date.tm_year = year - 1900;
            state.date.tm_mday = std::min(selectedDay, daysInMonth(month, year));
        }
        ImGui::PopStyleColor();

        // day-of-week headers
        static const char* dayHeaders[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.subtext0));
        for (int i = 0; i < 7; i++) {
            float textW = ImGui::CalcTextSize(dayHeaders[i]).x;
            float xOff = (CELL_W - textW) * 0.5f;
            if (i > 0)
                ImGui::SameLine(0, ITEM_GAP);
            ImGui::Dummy(ImVec2(xOff, 0));
            ImGui::SameLine(0, 0);
            ImGui::Text("%s", dayHeaders[i]);
            float remaining = CELL_W - xOff - textW;
            if (remaining > 0 && i < 6) {
                ImGui::SameLine(0, 0);
                ImGui::Dummy(ImVec2(remaining, 0));
            }
        }
        ImGui::PopStyleColor();

        // calendar grid
        int startCol = dayOfWeekSun0(1, month, year); // 0=Sun
        int totalDays = daysInMonth(month, year);
        float rowH = ImGui::GetFrameHeight();

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, CELL_W * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::S, Theme::Spacing::S));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ITEM_GAP, ITEM_GAP));

        int gridCol = 0;
        for (int i = 0; i < startCol; i++) {
            if (gridCol > 0)
                ImGui::SameLine(0, ITEM_GAP);
            ImGui::Dummy(ImVec2(CELL_W, rowH));
            gridCol++;
        }

        for (int d = 1; d <= totalDays; d++) {
            if (gridCol > 0)
                ImGui::SameLine(0, ITEM_GAP);

            bool isSel = (d == selectedDay);
            if (!isSel) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            } else {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            }

            ImGui::PushID(d);
            if (UIUtils::Button(std::to_string(d).c_str(), UIUtils::ButtonVariant::Secondary,
                                ImVec2(CELL_W, rowH))) {
                state.date.tm_mday = d;
                result.changed = true;
                if (!hasTime) {
                    result.committed = true;
                    result.value = format(state);
                }
            }
            ImGui::PopID();

            if (!isSel)
                ImGui::PopStyleColor(2);
            else
                ImGui::PopStyleVar();

            gridCol++;
            if (gridCol >= 7)
                gridCol = 0;
        }

        ImGui::PopStyleVar(3);
        ImGui::EndGroup();

        // --- right panel: time spinners ---
        if (hasTime) {
            ImGui::SameLine(0, Theme::Spacing::L);
            ImGui::BeginGroup();

            // header labels
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.subtext0));
            float labelPad = (SPINNER_W - ImGui::CalcTextSize("H").x) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelPad);
            ImGui::Text("H");
            ImGui::SameLine(0, SPINNER_W - ImGui::CalcTextSize("H").x - labelPad);
            labelPad = (SPINNER_W - ImGui::CalcTextSize("M").x) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelPad);
            ImGui::Text("M");
            ImGui::SameLine(0, SPINNER_W - ImGui::CalcTextSize("M").x - labelPad);
            labelPad = (SPINNER_W - ImGui::CalcTextSize("S").x) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelPad);
            ImGui::Text("S");
            ImGui::PopStyleColor();

            float itemH = ImGui::GetTextLineHeightWithSpacing();
            float spinnerH = itemH * 7;

            auto renderSpinner = [&](const char* childId, int& value, int maxVal) {
                // SetNextWindowScroll applies on child creation, avoiding the
                // first-frame delay that SetScrollY suffers when ScrollMax is
                // still 0 because content hasn't been laid out yet.
                if (state.scrollTime) {
                    float targetY = value * itemH - spinnerH * 0.5f + itemH * 0.5f;
                    ImGui::SetNextWindowScroll(ImVec2(-1.0f, std::max(0.0f, targetY)));
                }
                ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 9.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.0f);
                if (ImGui::BeginChild(childId, ImVec2(SPINNER_W, spinnerH), ImGuiChildFlags_None)) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                        ImVec2(Theme::Spacing::S, Theme::Spacing::S));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ITEM_GAP));
                    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));

                    for (int i = 0; i <= maxVal; i++) {
                        bool sel = (i == value);
                        ImGui::PushStyleColor(
                            ImGuiCol_Text, ImGui::GetColorU32(sel ? colors.text : colors.overlay0));

                        ImGui::PushID(i);
                        std::string label = std::format("{:02d}", i);
                        if (ImGui::Selectable(
                                label.c_str(), sel, 0,
                                ImVec2(SPINNER_W - Theme::Spacing::M, ImGui::GetFrameHeight()))) {
                            value = i;
                            result.changed = true;
                        }
                        ImGui::PopID();
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopStyleVar(3);
                }
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
            };

            renderSpinner("##dtp_h", state.hour, 23);
            ImGui::SameLine(0, 4);
            renderSpinner("##dtp_m", state.minute, 59);
            ImGui::SameLine(0, 4);
            renderSpinner("##dtp_s", state.second, 59);

            state.scrollTime = false;
            ImGui::EndGroup();
        }

        if (result.changed)
            result.value = format(state);

        return result;
    }

} // namespace DateTimePicker
