#include "ui/tab/sqlite_sequence_viewer_tab.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/file_database.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/spinner.hpp"
#include <cstdio>
#include <format>
#include <utility>

namespace {
    std::string makeTabName(const std::string& sequenceName) {
        return std::format("{} {}", ICON_FK_SORT_NUMERIC_ASC, sequenceName);
    }

    void readOnlyInput(const char* id, const std::string& value, float width) {
        ImGui::SetNextItemWidth(width);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
        // BeginDisabled prevents click/focus and dims to signal read-only.
        ImGui::BeginDisabled();
        ImGui::InputText(id, buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
    }

    void labeledRow(const char* label, const char* id, const std::string& value, float labelWidth,
                    float inputWidth) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        readOnlyInput(id, value, inputWidth);
    }
} // namespace

SQLiteSequenceViewerTab::SQLiteSequenceViewerTab(FileDatabase* db, std::string sequenceName)
    : Tab(makeTabName(sequenceName), TabType::SQLITE_SEQUENCE_VIEWER), db_(db),
      sequenceName_(std::move(sequenceName)) {}

void SQLiteSequenceViewerTab::render() {
    if (!loaded_ && !loadError_ && !fetchOp_.isRunning()) {
        fetchAsync();
    }
    checkFetchStatus();

    const auto& colors = Application::getInstance().getCurrentColors();

    if (fetchOp_.isRunning()) {
        ImGui::TextUnformatted("Loading...");
        ImGui::SameLine(0, Theme::Spacing::S);
        UIUtils::Spinner("##sqlite_seq_loading", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        return;
    }

    if (loadError_) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("Failed to load sequence: %s", loadErrorMessage_.c_str());
        ImGui::PopStyleColor();
        return;
    }

    constexpr float kLabelWidth = 110.0f;
    constexpr float kInputWidth = 220.0f;
    constexpr float kNumberWidth = 140.0f;
    constexpr float kWideNumberWidth = 200.0f;
    constexpr float kSmallNumberWidth = 80.0f;
    constexpr float kSmallLabelWidth = 90.0f;

    // top padding so the form doesn't hug the tab title bar
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));

    // breathing room between rows for the entire form
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));

    // left column: Name + Description
    ImGui::BeginGroup();
    {
        labeledRow("Name:", "##sqlite_seq_name", sequenceName_, kLabelWidth, kInputWidth);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Description:");
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(kInputWidth);
        char descBuf[2] = {'\0', '\0'};
        ImGui::BeginDisabled();
        ImGui::InputTextMultiline("##sqlite_seq_desc", descBuf, sizeof(descBuf),
                                  ImVec2(kInputWidth, ImGui::GetTextLineHeight() * 2.5f),
                                  ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, Theme::Spacing::XL);

    // middle column: Value, Min Value, Max Value
    ImGui::BeginGroup();
    {
        labeledRow("Value:", "##sqlite_seq_value", std::format("{}", value_), kLabelWidth,
                   kNumberWidth);
        labeledRow("Min Value:", "##sqlite_seq_min", std::format("{}", kMinValue), kLabelWidth,
                   kNumberWidth);
        labeledRow("Max Value:", "##sqlite_seq_max", std::format("{}", kMaxValue), kLabelWidth,
                   kWideNumberWidth);
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, Theme::Spacing::XL);

    // right column: Increment
    ImGui::BeginGroup();
    {
        labeledRow("Increment:", "##sqlite_seq_inc", std::format("{}", kIncrement),
                   kSmallLabelWidth, kSmallNumberWidth);
    }
    ImGui::EndGroup();

    ImGui::PopStyleVar();
}

void SQLiteSequenceViewerTab::fetchAsync() {
    if (!db_) {
        loadError_ = true;
        loadErrorMessage_ = "Database not available";
        return;
    }

    auto* db = db_;
    const std::string name = sequenceName_;

    fetchOp_.start([db, name]() -> FetchResult {
        FetchResult r;
        try {
            // escape single quotes for safe SQL literal
            std::string escaped;
            escaped.reserve(name.size());
            for (char c : name) {
                if (c == '\'') {
                    escaped += "''";
                } else {
                    escaped += c;
                }
            }
            const std::string sql =
                db->getDatabaseType() == DatabaseType::DUCKDB
                    ? std::format(
                          "SELECT last_value FROM duckdb_sequences() WHERE sequence_name = '{}'",
                          escaped)
                    : std::format("SELECT seq FROM sqlite_sequence WHERE name = '{}'", escaped);
            const QueryResult result = db->executeQuery(sql);
            if (!result.success()) {
                r.ok = false;
                r.error = result.errorMessage();
                return r;
            }
            if (!result.empty() && !result[0].tableData.empty() &&
                !result[0].tableData[0].empty()) {
                try {
                    r.value = std::stoll(result[0].tableData[0][0]);
                } catch (...) {
                    r.value = 0;
                }
            }
            r.ok = true;
            return r;
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
            return r;
        }
    });
}

void SQLiteSequenceViewerTab::checkFetchStatus() {
    fetchOp_.check([this](FetchResult r) {
        if (!r.ok) {
            loadError_ = true;
            loadErrorMessage_ = r.error;
            return;
        }
        value_ = r.value;
        loaded_ = true;
    });
}
