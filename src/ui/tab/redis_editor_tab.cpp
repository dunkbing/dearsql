#include "ui/tab/redis_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <chrono>
#include <format>

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
        const float editorHeight = paneHeight * splitterPosition_;
        const float resultsHeight = paneHeight * (1.0f - splitterPosition_) - 6.0f;

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

        renderVerticalSplitter("##redis_splitter", &splitterPosition_, 60.0f, 80.0f);

        if (ImGui::BeginChild("RedisResults", ImVec2(-1, resultsHeight), true,
                              ImGuiWindowFlags_NoScrollbar)) {
            renderToolbar();

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
    const bool running = queryOp_.isRunning();
    const auto& colors = Application::getInstance().getCurrentColors();

    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(colors.red));
        if (ImGui::Button(ICON_FA_STOP " Cancel")) {
            queryOp_.cancel();
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(colors.green));
        if (ImGui::Button(ICON_FA_PLAY " Run  (Ctrl+Enter)")) {
            command_ = editor_.GetText();
            startCommandExecutionAsync(command_);
        }
        ImGui::PopStyleColor();
    }

    if (lastQueryDuration_.count() > 0) {
        ImGui::SameLine(0, Theme::Spacing::L);
        ImGui::TextDisabled("%.2f ms", static_cast<double>(lastQueryDuration_.count()));
    }

    ImGui::Separator();
}

void RedisEditorTab::renderResults() const {
    if (queryResult_.empty()) {
        ImGui::TextDisabled("%s", LABEL_NO_RESULTS);
        return;
    }

    if (queryResult_.executionTimeMs > 0) {
        ImGui::Text("Execution time: %.2f ms", queryResult_.executionTimeMs);
    }

    const auto& r = queryResult_[0];

    if (!r.success) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", r.errorMessage.c_str());
        return;
    }

    if (r.columnNames.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", r.message.c_str());
        return;
    }

    if (r.tableData.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "(empty)");
        return;
    }

    const float tableHeight = std::max(ImGui::GetContentRegionAvail().y - 20.0f, 50.0f);

    TableRenderer::Config config;
    config.allowEditing = false;
    config.allowSelection = true;
    config.showRowNumbers = false;
    config.minHeight = tableHeight;

    TableRenderer tableRenderer(config);
    tableRenderer.setColumns(r.columnNames);
    tableRenderer.setData(r.tableData);
    tableRenderer.render("##redis_result");
}

void RedisEditorTab::startCommandExecutionAsync(const std::string& cmd) {
    if (queryOp_.isRunning() || !db_)
        return;

    queryError_.clear();
    lastQueryDuration_ = std::chrono::milliseconds{0};

    RedisDatabase* db = db_;
    queryOp_.startCancellable([cmd, db](const std::stop_token& stopToken) {
        QueryResult result;
        if (stopToken.stop_requested())
            return result;
        result = db->executeQuery(cmd);
        if (stopToken.stop_requested())
            return QueryResult{};
        return result;
    });
}

void RedisEditorTab::checkCommandExecutionStatus() {
    queryOp_.check([this](QueryResult result) {
        queryResult_ = std::move(result);
        if (!queryResult_.empty() && !queryResult_[0].success)
            queryError_ = queryResult_[0].errorMessage;
        lastQueryDuration_ =
            std::chrono::milliseconds{static_cast<long long>(queryResult_.executionTimeMs)};
    });
}

bool RedisEditorTab::renderVerticalSplitter(const char* id, float* position, float minSize1,
                                            float minSize2) const {
    constexpr float splitterThickness = 6.0f;
    const float totalHeight = ImGui::GetContentRegionAvail().y + splitterThickness;
    const float paneHeight = ImGui::GetWindowHeight();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 0.7f));

    ImGui::Button(id, ImVec2(-1, splitterThickness));
    const bool active = ImGui::IsItemActive();
    if (active) {
        const float delta = ImGui::GetIO().MouseDelta.y;
        const float newPos1 = *position * paneHeight + delta;
        const float newPos2 = (1.0f - *position) * paneHeight - delta;
        if (newPos1 >= minSize1 && newPos2 >= minSize2) {
            *position += delta / paneHeight;
        }
    }
    if (ImGui::IsItemHovered() || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    ImGui::PopStyleColor(3);
    return active;
}
