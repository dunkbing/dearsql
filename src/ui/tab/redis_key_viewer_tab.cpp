#include "ui/tab/redis_key_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <format>

RedisKeyViewerTab::RedisKeyViewerTab(const std::string& name, RedisDatabase* db,
                                     const std::string& pattern)
    : Tab(name, TabType::REDIS_KEY_VIEWER), db_(db), pattern_(pattern) {
    loadDataAsync();
}

RedisKeyViewerTab::~RedisKeyViewerTab() = default;

void RedisKeyViewerTab::loadDataAsync() {
    if (isLoading_ || !db_)
        return;

    isLoading_ = true;
    hasError_ = false;
    loadingError_.clear();

    RedisDatabase* db = db_;
    const std::string pattern = pattern_;
    const int limit = rowsPerPage_;
    const int offset = currentPage_ * rowsPerPage_;

    loadOp_.start([db, pattern, limit, offset] {
        auto cols = db->getColumnNames(pattern);
        auto data = db->getTableData(pattern, limit, offset);
        return std::make_pair(std::move(cols), std::move(data));
    });
}

void RedisKeyViewerTab::checkLoadStatus() {
    loadOp_.check([this](auto result) {
        isLoading_ = false;
        columnNames_ = std::move(result.first);
        tableData_ = std::move(result.second);

        if (static_cast<int>(tableData_.size()) < rowsPerPage_) {
            totalRows_ = currentPage_ * rowsPerPage_ + static_cast<int>(tableData_.size());
        } else {
            totalRows_ = -1; // more pages may exist
        }
    });
}

void RedisKeyViewerTab::render() {
    checkLoadStatus();

    const auto& colors = Application::getInstance().getCurrentColors();
    const std::string displayName = (pattern_ == "*") ? "All Keys" : pattern_;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);

    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.red));
    ImGui::Text(ICON_FA_DATABASE);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);
    if (db_) {
        const auto& connInfo = db_->getConnectionInfo();
        ImGui::Text("%s:%d", connInfo.host.c_str(), connInfo.port);
        ImGui::SameLine(0, Theme::Spacing::L);
    }
    ImGui::Text(ICON_FA_KEY " %s", displayName.c_str());
    ImGui::Separator();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);

    renderToolbar();

    if (isLoading_) {
        const ImVec2 winPos = ImGui::GetWindowPos();
        const float cx = winPos.x + ImGui::GetWindowWidth() * 0.5f;
        const float cy = winPos.y + ImGui::GetWindowHeight() * 0.5f;
        constexpr float r = 10.0f;
        ImGui::SetCursorScreenPos(ImVec2(cx - r, cy - r));
        UIUtils::Spinner("##redis_key_load", r, 2, ImGui::GetColorU32(ImGuiCol_Text));
        return;
    }

    if (hasError_) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", loadingError_.c_str());
        return;
    }

    if (tableData_.empty()) {
        ImGui::TextDisabled("No keys found for pattern: %s", pattern_.c_str());
        return;
    }

    if (totalRows_ >= 0) {
        ImGui::Text("Rows: %d", totalRows_);
    } else {
        ImGui::Text("Rows: %zu  (page %d, more available)", tableData_.size(), currentPage_ + 1);
    }

    const float tableHeight = std::max(ImGui::GetContentRegionAvail().y - 4.0f, 50.0f);

    TableRenderer::Config config;
    config.allowEditing = false;
    config.allowSelection = true;
    config.showRowNumbers = true;
    config.minHeight = tableHeight;

    TableRenderer tableRenderer(config);
    tableRenderer.setColumns(columnNames_);
    tableRenderer.setData(tableData_);
    tableRenderer.render("##redis_keys_table");
}

void RedisKeyViewerTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);

    if (isLoading_)
        ImGui::BeginDisabled();

    if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Refresh")) {
        currentPage_ = 0;
        loadDataAsync();
    }

    if (currentPage_ > 0) {
        ImGui::SameLine(0, Theme::Spacing::S);
        if (ImGui::Button(ICON_FA_ANGLE_LEFT " Prev")) {
            --currentPage_;
            loadDataAsync();
        }
    }

    const bool hasMore = totalRows_ < 0 && static_cast<int>(tableData_.size()) >= rowsPerPage_;
    if (hasMore) {
        ImGui::SameLine(0, Theme::Spacing::S);
        if (ImGui::Button("Next " ICON_FA_ANGLE_RIGHT)) {
            ++currentPage_;
            loadDataAsync();
        }
    }

    if (isLoading_)
        ImGui::EndDisabled();

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::Separator();
}
