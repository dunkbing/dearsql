#pragma once

#include "imgui.h"
#include "utils/chart_data.hpp"
#include <string>
#include <vector>

// the chart tab under a grid: picks a chart from the columns, lets the user change it,
// and draws it with the draw list
class ResultChart {
public:
    void render(const char* id, const std::vector<std::string>& columns,
                const std::vector<std::vector<std::string>>& rows);
    // rows changed in place (an edit)
    void invalidate() {
        rows_ = nullptr;
    }

    // for tests
    [[nodiscard]] bool visible() const {
        return lastFrame_ >= ImGui::GetFrameCount() - 1;
    }
    [[nodiscard]] const chart::Settings& settings() const {
        return settings_;
    }
    [[nodiscard]] const chart::Data& data() const {
        return data_;
    }
    void setSettings(const chart::Settings& settings) {
        settings_ = settings;
        dirty_ = true;
    }

private:
    void renderControls();
    void renderCartesian(const ImVec2& size);
    void renderPie(const ImVec2& size);
    void rebuild(const std::vector<std::string>& columns,
                 const std::vector<std::vector<std::string>>& rows);

    chart::Settings settings_;
    chart::Data data_;
    std::vector<std::string> columns_;
    std::vector<bool> numeric_;
    const void* rows_ = nullptr;
    size_t size_ = 0;
    bool dirty_ = true;
    int lastFrame_ = -10;
    float maxLabelWidth_ = 0;
};
