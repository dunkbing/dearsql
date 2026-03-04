#include "ui/tab/redis_key_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <format>

namespace {
    // columns: Key(0), Type(1), Value(2), TTL(3), Size(4)
    constexpr int COL_KEY = 0;
    constexpr int COL_TYPE = 1;
    constexpr int COL_VALUE = 2;
    constexpr int COL_TTL = 3;
    constexpr int COL_SIZE = 4;

    int64_t parseTtlString(const std::string& s) {
        try {
            return std::stoll(s);
        } catch (const std::exception&) {
            return -2; // parse error
        }
    }
} // namespace

RedisKeyViewerTab::RedisKeyViewerTab(const std::string& name, RedisDatabase* db,
                                     const std::string& pattern)
    : Tab(name, TabType::REDIS_KEY_VIEWER), db_(db), pattern_(pattern) {
    initializeTableRenderer();
    loadDataAsync();
}

RedisKeyViewerTab::~RedisKeyViewerTab() = default;

void RedisKeyViewerTab::initializeTableRenderer() {
    TableRenderer::Config config;
    config.allowEditing = true;
    config.allowSelection = true;
    config.showRowNumbers = true;
    config.minHeight = 200.0f;
    config.nonEditableColumns = {COL_SIZE};
    config.columnInputFlags = {{COL_TTL, ImGuiInputTextFlags_CharsDecimal}};
    config.columnDropdownOptions = {
        {COL_TYPE, {"string", "list", "set", "zset", "hash", "stream"}}};

    tableRenderer_ = std::make_unique<TableRenderer>(config);

    tableRenderer_->setOnCellEdit([this](int row, int col, const std::string& newValue) {
        if (row < 0 || row >= static_cast<int>(tableData_.size()))
            return;
        if (newValue != tableData_[row][col]) {
            tableData_[row][col] = newValue;
            hasChanges_ = true;
            if (row < static_cast<int>(editedCells_.size()) &&
                col < static_cast<int>(editedCells_[row].size())) {
                editedCells_[row][col] = true;
            }
        }
    });

    tableRenderer_->setOnCellSelect([this](int row, int col) {
        selectedRow_ = row;
        selectedCol_ = col;
    });

    // disable type editing for existing keys
    tableRenderer_->setCellEditableCallback([this](int row, int col) -> bool {
        if (col == COL_TYPE && row < static_cast<int>(isNewRow_.size()) && !isNewRow_[row])
            return false;
        return true;
    });

    // color-code the Type column
    tableRenderer_->setCellColorCallback(
        [](int /*row*/, int col, const std::string& value) -> unsigned int {
            if (col != COL_TYPE)
                return 0;
            const auto& colors = Application::getInstance().getCurrentColors();
            if (value == "string")
                return ImGui::GetColorU32(colors.green);
            if (value == "list")
                return ImGui::GetColorU32(colors.blue);
            if (value == "set")
                return ImGui::GetColorU32(colors.mauve);
            if (value == "zset")
                return ImGui::GetColorU32(colors.peach);
            if (value == "hash")
                return ImGui::GetColorU32(colors.yellow);
            if (value == "stream")
                return ImGui::GetColorU32(colors.teal);
            return ImGui::GetColorU32(colors.subtext0);
        });
}

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
        originalData_ = tableData_;
        editedCells_ = std::vector<std::vector<bool>>(
            tableData_.size(), std::vector<bool>(columnNames_.size(), false));
        isNewRow_ = std::vector<bool>(tableData_.size(), false);
        hasChanges_ = false;
        selectedRow_ = -1;
        selectedCol_ = -1;

        if (static_cast<int>(tableData_.size()) < rowsPerPage_) {
            totalRows_ = currentPage_ * rowsPerPage_ + static_cast<int>(tableData_.size());
        } else {
            totalRows_ = -1; // more pages may exist
        }
    });

    saveOp_.check([this](SaveResult result) {
        if (result.success) {
            saveError_.clear();
            loadDataAsync();
        } else {
            saveError_ = result.errorMessage;
        }
    });
}

void RedisKeyViewerTab::render() {
    checkLoadStatus();

    const auto& colors = Application::getInstance().getCurrentColors();
    const std::string displayName = (pattern_ == "*") ? "Browse" : pattern_;

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

    if (!saveError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.red));
        ImGui::TextWrapped("%s", saveError_.c_str());
        ImGui::PopStyleColor();
    }

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

    tableRenderer_->setColumns(columnNames_);
    tableRenderer_->setData(tableData_);
    tableRenderer_->setCellEditedStatus(editedCells_);
    tableRenderer_->setSelectedCell(selectedRow_, selectedCol_);
    tableRenderer_->setRowNumberOffset(currentPage_ * rowsPerPage_);
    tableRenderer_->render("##redis_keys_table");
}

void RedisKeyViewerTab::saveChanges() {
    if (!hasChanges_ || !db_ || saveOp_.isRunning())
        return;

    std::vector<std::string> commands;

    for (int row = 0; row < static_cast<int>(editedCells_.size()); row++) {
        const auto& currRow = tableData_[row];

        if (row < static_cast<int>(isNewRow_.size()) && isNewRow_[row]) {
            // new key
            const std::string& key = currRow[COL_KEY];
            const std::string& type = currRow[COL_TYPE];
            const std::string& value = currRow[COL_VALUE];
            if (key.empty())
                continue;

            if (type == "list") {
                commands.push_back(std::format("RPUSH {} {}", key, value));
            } else if (type == "set") {
                commands.push_back(std::format("SADD {} {}", key, value));
            } else if (type == "zset") {
                commands.push_back(std::format("ZADD {} 0 {}", key, value));
            } else if (type == "hash") {
                commands.push_back(std::format("HSET {} {} \"\"", key, value));
            } else {
                commands.push_back(std::format("SET {} {}", key, value));
            }

            int64_t ttl = parseTtlString(currRow[COL_TTL]);
            if (ttl > 0) {
                commands.push_back(std::format("EXPIRE {} {}", key, ttl));
            }
        } else {
            // existing key
            const auto& origRow = originalData_[row];
            const std::string& origKey = origRow[COL_KEY];
            std::string activeKey = origKey;

            if (editedCells_[row][COL_KEY] && currRow[COL_KEY] != origKey) {
                commands.push_back(std::format("RENAME {} {}", origKey, currRow[COL_KEY]));
                activeKey = currRow[COL_KEY];
            }

            if (editedCells_[row][COL_VALUE] && currRow[COL_VALUE] != origRow[COL_VALUE]) {
                commands.push_back(std::format("SET {} {}", activeKey, currRow[COL_VALUE]));
            }

            if (editedCells_[row][COL_TTL] && currRow[COL_TTL] != origRow[COL_TTL]) {
                int64_t ttl = parseTtlString(currRow[COL_TTL]);
                if (ttl == -1) {
                    commands.push_back(std::format("PERSIST {}", activeKey));
                } else if (ttl > 0) {
                    commands.push_back(std::format("EXPIRE {} {}", activeKey, ttl));
                }
            }
        }
    }

    if (commands.empty()) {
        hasChanges_ = false;
        return;
    }

    RedisDatabase* db = db_;
    saveOp_.start([commands = std::move(commands), db]() -> SaveResult {
        for (const auto& cmd : commands) {
            auto result = db->executeQuery(cmd);
            if (!result.empty() && !result[0].success) {
                return {false, std::format("'{}': {}", cmd, result[0].errorMessage)};
            }
        }
        return {true, ""};
    });
}

void RedisKeyViewerTab::cancelChanges() {
    // remove new rows from originalData_ (iterate backwards to preserve indices)
    for (int i = static_cast<int>(isNewRow_.size()) - 1; i >= 0; i--) {
        if (isNewRow_[i]) {
            originalData_.erase(originalData_.begin() + i);
        }
    }
    tableData_ = originalData_;
    editedCells_ = std::vector<std::vector<bool>>(tableData_.size(),
                                                  std::vector<bool>(columnNames_.size(), false));
    isNewRow_ = std::vector<bool>(tableData_.size(), false);
    hasChanges_ = false;
    saveError_.clear();
}

void RedisKeyViewerTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);

    const bool busy = isLoading_ || saveOp_.isRunning();
    if (busy)
        ImGui::BeginDisabled();

    if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Refresh")) {
        currentPage_ = 0;
        hasChanges_ = false;
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

    // add key
    ImGui::SameLine(0, Theme::Spacing::S);
    if (ImGui::Button(ICON_FA_PLUS " Add key")) {
        const int numCols = static_cast<int>(columnNames_.size());
        std::vector<std::string> newRow = {"", "string", "", "-1", "-"};
        newRow.resize(numCols);

        int insertIdx =
            (selectedRow_ >= 0) ? selectedRow_ + 1 : static_cast<int>(tableData_.size());
        tableData_.insert(tableData_.begin() + insertIdx, newRow);
        originalData_.insert(originalData_.begin() + insertIdx, std::vector<std::string>(numCols));
        editedCells_.insert(editedCells_.begin() + insertIdx, std::vector<bool>(numCols, true));
        isNewRow_.insert(isNewRow_.begin() + insertIdx, true);
        hasChanges_ = true;
        selectedRow_ = insertIdx;
        selectedCol_ = COL_KEY;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add new key");

    // save/cancel
    ImGui::SameLine(0, Theme::Spacing::L);
    if (hasChanges_) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK)) {
            saveChanges();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save");

        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        if (ImGui::Button(ICON_FA_XMARK)) {
            cancelChanges();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard changes");

        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::TextColored(colors.peach, "Unsaved changes");
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_FLOPPY_DISK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Save");

        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_XMARK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Discard changes");
    }

    if (busy)
        ImGui::EndDisabled();

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::Separator();
}
