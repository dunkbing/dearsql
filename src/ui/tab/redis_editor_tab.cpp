#include "ui/tab/redis_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace {
    constexpr const char* LABEL_RUNNING = "Running command...";
    constexpr const char* LABEL_NO_RESULTS = "No results. Execute a command to see output here.";

    static const std::vector<std::string> REDIS_COMPLETION_KEYWORDS = {
        "APPEND",
        "AUTH",
        "BGSAVE",
        "BITCOUNT",
        "BITFIELD",
        "BITOP",
        "BITPOS",
        "BLPOP",
        "BRPOP",
        "CLIENT",
        "CLUSTER",
        "COMMAND",
        "CONFIG",
        "COPY",
        "DBSIZE",
        "DEBUG",
        "DECR",
        "DECRBY",
        "DEL",
        "DISCARD",
        "DUMP",
        "ECHO",
        "EVAL",
        "EVALSHA",
        "EXEC",
        "EXISTS",
        "EXPIRE",
        "EXPIREAT",
        "EXPIRETIME",
        "FLUSHALL",
        "FLUSHDB",
        "GEOADD",
        "GEODIST",
        "GEOHASH",
        "GEOPOS",
        "GEOSEARCH",
        "GET",
        "GETBIT",
        "GETDEL",
        "GETEX",
        "GETRANGE",
        "GETSET",
        "HDEL",
        "HELLO",
        "HEXISTS",
        "HGET",
        "HGETALL",
        "HINCRBY",
        "HINCRBYFLOAT",
        "HKEYS",
        "HLEN",
        "HMGET",
        "HMSET",
        "HRANDFIELD",
        "HSCAN",
        "HSET",
        "HSETNX",
        "HSTRLEN",
        "HVALS",
        "INCR",
        "INCRBY",
        "INCRBYFLOAT",
        "INFO",
        "KEYS",
        "LASTSAVE",
        "LCS",
        "LINDEX",
        "LINSERT",
        "LLEN",
        "LMOVE",
        "LMPOP",
        "LPOP",
        "LPOS",
        "LPUSH",
        "LPUSHX",
        "LRANGE",
        "LREM",
        "LSET",
        "LTRIM",
        "MEMORY",
        "MGET",
        "MIGRATE",
        "MODULE",
        "MONITOR",
        "MOVE",
        "MSET",
        "MSETNX",
        "MULTI",
        "OBJECT",
        "PERSIST",
        "PEXPIRE",
        "PEXPIREAT",
        "PFADD",
        "PFCOUNT",
        "PFMERGE",
        "PING",
        "PSETEX",
        "PSUBSCRIBE",
        "PTTL",
        "PUBLISH",
        "PUBSUB",
        "PUNSUBSCRIBE",
        "QUIT",
        "RANDOMKEY",
        "READONLY",
        "READWRITE",
        "RENAME",
        "RENAMENX",
        "REPLICAOF",
        "RESET",
        "RESTORE",
        "ROLE",
        "RPOP",
        "RPOPLPUSH",
        "RPUSH",
        "RPUSHX",
        "SADD",
        "SAVE",
        "SCAN",
        "SCARD",
        "SCRIPT",
        "SDIFF",
        "SDIFFSTORE",
        "SELECT",
        "SET",
        "SETBIT",
        "SETEX",
        "SETNX",
        "SETRANGE",
        "SINTER",
        "SINTERCARD",
        "SINTERSTORE",
        "SLAVEOF",
        "SLOWLOG",
        "SMEMBERS",
        "SMISMEMBER",
        "SMOVE",
        "SORT",
        "SORT_RO",
        "SPOP",
        "SRANDMEMBER",
        "SREM",
        "SSCAN",
        "STRLEN",
        "SUBSCRIBE",
        "SUNION",
        "SUNIONSTORE",
        "SWAPDB",
        "SYNC",
        "TIME",
        "TTL",
        "TYPE",
        "UNLINK",
        "UNSUBSCRIBE",
        "UNWATCH",
        "WAIT",
        "WATCH",
        "XACK",
        "XADD",
        "XAUTOCLAIM",
        "XCLAIM",
        "XDEL",
        "XGROUP",
        "XINFO",
        "XLEN",
        "XPENDING",
        "XRANGE",
        "XREAD",
        "XREADGROUP",
        "XREVRANGE",
        "XSETID",
        "XTRIM",
        "ZADD",
        "ZCARD",
        "ZCOUNT",
        "ZDIFF",
        "ZDIFFSTORE",
        "ZINCRBY",
        "ZINTER",
        "ZINTERCARD",
        "ZINTERSTORE",
        "ZLEXCOUNT",
        "ZMPOP",
        "ZMSCORE",
        "ZPOPMAX",
        "ZPOPMIN",
        "ZRANDMEMBER",
        "ZRANGE",
        "ZRANGEBYLEX",
        "ZRANGEBYSCORE",
        "ZRANGESTORE",
        "ZRANK",
        "ZREM",
        "ZREMRANGEBYLEX",
        "ZREMRANGEBYSCORE",
        "ZREMRANGEBYRANK",
        "ZREVRANGE",
        "ZREVRANGEBYLEX",
        "ZREVRANGEBYSCORE",
        "ZREVRANK",
        "ZSCAN",
        "ZSCORE",
        "ZUNION",
        "ZUNIONSTORE",
    };

    // split multi-line input into individual commands
    std::vector<std::string> splitCommands(const std::string& input) {
        std::vector<std::string> commands;
        std::istringstream stream(input);
        std::string line;
        while (std::getline(stream, line)) {
            auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos)
                continue;
            auto end = line.find_last_not_of(" \t\r");
            commands.push_back(line.substr(start, end - start + 1));
        }
        return commands;
    }

    std::string currentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&time, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return buf;
    }
} // namespace

RedisEditorTab::RedisEditorTab(const std::string& name, RedisDatabase* db)
    : Tab(name, TabType::REDIS_EDITOR), db_(db) {
    editor_.SetShowLineNumbers(false);
    editor_.SetLanguage(dearsql::TextEditor::Language::Redis);
    editor_.SetCompletionKeywords(REDIS_COMPLETION_KEYWORDS);
    editor_.SetSubmitCallback([this] {
        command_ = editor_.GetText();
        startCommandExecutionAsync(command_);
    });
}

RedisEditorTab::~RedisEditorTab() = default;

void RedisEditorTab::render() {
    const bool dark = Application::getInstance().isDarkTheme();
    editor_.SetPalette(
        dearsql::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));

    checkCommandExecutionStatus();

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::S);
    renderHeader();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);

    totalContentHeight_ = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginChild("##redis_left_pane", ImVec2(-1, totalContentHeight_), false)) {
        const float paneHeight = ImGui::GetContentRegionAvail().y;
        const float toolbarHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::S;
        const float editorHeight = paneHeight * splitterPosition_;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition_) - 6.0f - toolbarHeight;

        if (ImGui::BeginChild("RedisEditor", ImVec2(-1, editorHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            if (pendingFocusFrames_ > 0) {
                editor_.SetFocus();
                --pendingFocusFrames_;
            }
            editor_.Render("##Redis", ImVec2(-1, -1), true);
            command_ = editor_.GetText();
        }
        ImGui::EndChild();

        renderToolbar();
        renderVerticalSplitter("##redis_splitter", &splitterPosition_, 60.0f, 80.0f);

        if (ImGui::BeginChild("RedisResults", ImVec2(-1, resultsHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            const ImVec2 contentStart = ImGui::GetCursorScreenPos();
            if (queryOp_.isRunning())
                ImGui::BeginDisabled();
            renderResults();
            if (queryOp_.isRunning())
                ImGui::EndDisabled();

            // spinner overlay while running
            if (queryOp_.isRunning()) {
                const ImVec2 winPos = ImGui::GetWindowPos();
                const ImVec2 winSize = ImGui::GetWindowSize();
                const ImVec2 overlayEnd(winPos.x + winSize.x, winPos.y + winSize.y);

                const auto& colors = Application::getInstance().getCurrentColors();
                ImVec4 bg = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(colors.base));
                bg.w = 0.75f;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(contentStart, overlayEnd, ImGui::GetColorU32(bg));

                const float cx = (contentStart.x + overlayEnd.x) * 0.5f;
                const float cy = (contentStart.y + overlayEnd.y) * 0.5f;
                constexpr float spinnerRadius = 10.0f;
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - spinnerRadius, cy - spinnerRadius - Theme::Spacing::M));
                UIUtils::Spinner("##redis_spinner", spinnerRadius, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));

                const char* loadingText = LABEL_RUNNING;
                const ImVec2 textSize = ImGui::CalcTextSize(loadingText);
                ImGui::SetCursorScreenPos(
                    ImVec2(cx - textSize.x * 0.5f, cy + spinnerRadius + Theme::Spacing::S));
                ImGui::Text("%s", loadingText);
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void RedisEditorTab::renderHeader() const {
    if (!db_) {
        ImGui::Text("Redis (no connection)");
        ImGui::Separator();
        return;
    }

    const auto& connInfo = db_->getConnectionInfo();
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.red));
    ImGui::Text(ICON_FA_DATABASE);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, Theme::Spacing::S);
    ImGui::Text("%s:%d", connInfo.host.c_str(), connInfo.port);

    ImGui::SameLine(0, Theme::Spacing::L);
    if (db_->isConnected()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.green));
        ImGui::Text(ICON_FA_CIRCLE " Connected");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.red));
        ImGui::Text(ICON_FA_CIRCLE_EXCLAMATION " Disconnected");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
}

void RedisEditorTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);

    if (queryOp_.isRunning()) {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_PLAY " Run");
        ImGui::EndDisabled();

        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(ICON_FA_STOP " Cancel")) {
            queryOp_.cancel();
        }
    } else {
        if (ImGui::Button(ICON_FA_PLAY " Run")) {
            command_ = editor_.GetText();
            startCommandExecutionAsync(command_);
        }
    }

    if (!resultHistory_.empty()) {
        ImGui::SameLine(0, Theme::Spacing::M);
        if (ImGui::Button(ICON_FA_TRASH_CAN " Clear")) {
            resultHistory_.clear();
        }
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SameLine(0, Theme::Spacing::L);
    ImGui::Checkbox("Auto clear", &autoClearEditor_);
}

void RedisEditorTab::renderResults() {
    if (resultHistory_.empty()) {
        ImGui::TextDisabled("%s", LABEL_NO_RESULTS);
        return;
    }

    const float tableHeight = std::max(ImGui::GetContentRegionAvail().y, 50.0f);
    const auto& colors = Application::getInstance().getCurrentColors();

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_NoHostExtendX;

    if (ImGui::BeginTable("##redis_history", 7, flags, ImVec2(-1, tableHeight))) {
        ImGui::TableSetupColumn("cmd", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("result", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("spacer", ImGuiTableColumnFlags_WidthFixed, Theme::Spacing::M);
        ImGui::TableSetupColumn("duration", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("rerun", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("delete", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        // no header row

        int deleteIdx = -1;
        std::string rerunCmd;

        // render newest first
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(resultHistory_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                // map to reverse order
                int idx = static_cast<int>(resultHistory_.size()) - 1 - row;
                const auto& entry = resultHistory_[idx];

                ImGui::TableNextRow();
                ImGui::PushID(idx);

                // command
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", entry.command.c_str());

                // result
                ImGui::TableNextColumn();
                if (entry.success) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.green));
                    ImGui::TextWrapped("%s", entry.result.c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.red));
                    ImGui::TextWrapped("%s", entry.errorMessage.c_str());
                    ImGui::PopStyleColor();
                }

                // time
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", entry.timestamp.c_str());

                // spacer
                ImGui::TableNextColumn();

                // duration
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.2f ms", entry.durationMs);

                // re-run
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_PLAY)) {
                    rerunCmd = entry.command;
                }

                // delete
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_XMARK)) {
                    deleteIdx = idx;
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();

        // handle actions after table rendering
        if (deleteIdx >= 0 && deleteIdx < static_cast<int>(resultHistory_.size())) {
            resultHistory_.erase(resultHistory_.begin() + deleteIdx);
        }
        if (!rerunCmd.empty()) {
            startCommandExecutionAsync(rerunCmd);
        }
    }
}

void RedisEditorTab::startCommandExecutionAsync(const std::string& cmd) {
    if (queryOp_.isRunning() || !db_)
        return;

    auto commands = splitCommands(cmd);
    if (commands.empty())
        return;

    if (autoClearEditor_) {
        editor_.SetText("");
        command_.clear();
    }

    RedisDatabase* db = db_;
    queryOp_.startCancellable(
        [commands = std::move(commands), db](const std::stop_token& stopToken) {
            std::vector<RedisResultEntry> entries;
            std::string ts = currentTimestamp();

            for (const auto& command : commands) {
                if (stopToken.stop_requested())
                    break;

                auto result = db->executeQuery(command);

                RedisResultEntry entry;
                entry.command = command;
                entry.durationMs = result.executionTimeMs;
                entry.timestamp = ts;

                if (!result.empty() && result[0].success) {
                    if (!result[0].tableData.empty() && !result[0].tableData[0].empty()) {
                        entry.result = result[0].tableData[0][0];
                    } else {
                        entry.result = "OK";
                    }
                    entry.success = true;
                } else if (!result.empty()) {
                    entry.success = false;
                    entry.errorMessage = result[0].errorMessage;
                } else {
                    entry.success = false;
                    entry.errorMessage = "No result";
                }

                entries.push_back(std::move(entry));
            }
            return entries;
        });
}

void RedisEditorTab::checkCommandExecutionStatus() {
    queryOp_.check([this](std::vector<RedisResultEntry> entries) {
        for (auto& entry : entries) {
            resultHistory_.push_back(std::move(entry));
        }
        // cap history to prevent unbounded growth
        constexpr size_t maxHistory = 1000;
        if (resultHistory_.size() > maxHistory) {
            resultHistory_.erase(resultHistory_.begin(),
                                 resultHistory_.begin() +
                                     static_cast<ptrdiff_t>(resultHistory_.size() - maxHistory));
        }
    });
}

bool RedisEditorTab::renderVerticalSplitter(const char* id, float* position, float minSize1,
                                            float minSize2) const {
    constexpr float hoverThickness = 6.0f;

    ImGui::InvisibleButton(id, ImVec2(-1, hoverThickness));

    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    if (hovered || held) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    bool changed = false;
    if (held) {
        const float delta = ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f) {
            const float availableHeight = totalContentHeight_;
            const float currentPixelPos = *position * availableHeight;
            const float newPixelPos = currentPixelPos + delta;
            float newPosition = newPixelPos / availableHeight;

            const float minPos1 = minSize1 / availableHeight;
            const float maxPos1 = 1.0f - (minSize2 / availableHeight);

            newPosition = std::max(minPos1, std::min(maxPos1, newPosition));

            if (newPosition != *position) {
                *position = newPosition;
                changed = true;
            }
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetItemRectMin();
    const ImVec2 size = ImGui::GetItemRectSize();
    const float centerY = pos.y + size.y / 2.0f;
    const ImU32 color = (hovered || held) ? ImGui::GetColorU32(ImGuiCol_SeparatorHovered)
                                          : ImGui::GetColorU32(ImGuiCol_Separator);
    drawList->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + size.x, centerY), color, 2.0f);

    return changed;
}
