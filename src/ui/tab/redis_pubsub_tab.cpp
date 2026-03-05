#include "ui/tab/redis_pubsub_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <hiredis/hiredis.h>
#include <hiredis/hiredis_ssl.h>
#include <poll.h>

RedisPubSubTab::RedisPubSubTab(const std::string& name, RedisDatabase* db)
    : Tab(name, TabType::REDIS_PUBSUB), db_(db), statusPanel_(db) {
    strncpy(publishChannelBuf_, "*", sizeof(publishChannelBuf_) - 1);
}

RedisPubSubTab::~RedisPubSubTab() {
    unsubscribe();
}

void RedisPubSubTab::setSubError(std::string error) {
    std::lock_guard lock(subErrorMutex_);
    subError_ = std::move(error);
}

void RedisPubSubTab::clearSubError() {
    std::lock_guard lock(subErrorMutex_);
    subError_.clear();
}

std::string RedisPubSubTab::getSubError() const {
    std::lock_guard lock(subErrorMutex_);
    return subError_;
}

redisContext* RedisPubSubTab::createSubscriberContext() {
    const auto& info = db_->getConnectionInfo();

    constexpr timeval timeout = {5, 0};
    auto* ctx = redisConnectWithTimeout(info.host.c_str(), info.port, timeout);
    if (!ctx || ctx->err) {
        setSubError(ctx ? ctx->errstr : "Failed to allocate subscriber context");
        if (ctx)
            redisFree(ctx);
        return nullptr;
    }

    // TLS
    if (info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
        info.sslmode == SslMode::VerifyFull) {
        redisSSLContextError sslErr = REDIS_SSL_CTX_NONE;
        const char* caPath = (!info.sslCACertPath.empty() && (info.sslmode == SslMode::VerifyCA ||
                                                              info.sslmode == SslMode::VerifyFull))
                                 ? info.sslCACertPath.c_str()
                                 : nullptr;

        subSslCtx_ = redisCreateSSLContext(caPath, nullptr, nullptr, nullptr, nullptr, &sslErr);
        if (!subSslCtx_) {
            setSubError(std::string("Subscriber TLS context failed: ") +
                        redisSSLContextGetError(sslErr));
            redisFree(ctx);
            return nullptr;
        }
        if (redisInitiateSSLWithContext(ctx, subSslCtx_) != REDIS_OK) {
            setSubError(ctx->errstr[0] ? ctx->errstr : "Subscriber TLS handshake failed");
            redisFree(ctx);
            redisFreeSSLContext(subSslCtx_);
            subSslCtx_ = nullptr;
            return nullptr;
        }
    }

    // AUTH
    if (!info.password.empty()) {
        redisReply* reply = nullptr;
        if (!info.username.empty()) {
            reply = static_cast<redisReply*>(
                redisCommand(ctx, "AUTH %s %s", info.username.c_str(), info.password.c_str()));
        } else {
            reply = static_cast<redisReply*>(redisCommand(ctx, "AUTH %s", info.password.c_str()));
        }
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            setSubError(reply ? reply->str : "Subscriber auth failed");
            if (reply)
                freeReplyObject(reply);
            redisFree(ctx);
            if (subSslCtx_) {
                redisFreeSSLContext(subSslCtx_);
                subSslCtx_ = nullptr;
            }
            return nullptr;
        }
        freeReplyObject(reply);
    }

    return ctx;
}

void RedisPubSubTab::subscribe(const std::string& pattern) {
    auto cur = subState_.load();
    if (cur == SubState::Subscribed || cur == SubState::Subscribing)
        return;

    // clean up stale resources from a previous error/stop before starting again
    unsubscribe();
    subState_.store(SubState::Subscribing);
    clearSubError();
    activePattern_ = pattern;

    {
        std::lock_guard lock(messageMutex_);
        pendingMessages_.clear();
    }
    displayMessages_.clear();
    totalMessageCount_.store(0);

    subContext_ = createSubscriberContext();
    if (!subContext_) {
        subState_.store(SubState::Error);
        return;
    }

    subThread_ = std::jthread([this](std::stop_token st) { subscriberLoop(st); });
}

void RedisPubSubTab::subscriberLoop(std::stop_token stopToken) {
    // use PSUBSCRIBE for glob patterns, SUBSCRIBE for exact channel
    bool isPattern = activePattern_.find_first_of("*?[") != std::string::npos;
    const char* cmd = isPattern ? "PSUBSCRIBE" : "SUBSCRIBE";

    auto* reply =
        static_cast<redisReply*>(redisCommand(subContext_, "%s %s", cmd, activePattern_.c_str()));
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        setSubError(reply ? reply->str : "Subscribe command failed");
        if (reply)
            freeReplyObject(reply);
        subState_.store(SubState::Error);
        return;
    }
    freeReplyObject(reply);
    subState_.store(SubState::Subscribed);

    // poll the socket instead of redisSetTimeout (which permanently errors the context)
    struct pollfd pfd = {.fd = subContext_->fd, .events = POLLIN, .revents = 0};

    while (!stopToken.stop_requested()) {
        int pollRc = poll(&pfd, 1, 100); // 100ms timeout
        if (pollRc == 0)
            continue; // timeout, check stop token
        if (pollRc < 0) {
            if (errno == EINTR)
                continue;
            setSubError("poll error: " + std::string(strerror(errno)));
            subState_.store(SubState::Error);
            break;
        }

        redisReply* msg = nullptr;
        int rc = redisGetReply(subContext_, reinterpret_cast<void**>(&msg));
        if (rc == REDIS_ERR) {
            setSubError(subContext_->errstr);
            subState_.store(SubState::Error);
            break;
        }

        if (!msg)
            continue;

        if (msg->type == REDIS_REPLY_ARRAY && msg->elements >= 3) {
            std::string msgType = msg->element[0]->str;
            std::string channel;
            std::string payload;

            if (msgType == "pmessage" && msg->elements >= 4) {
                channel = msg->element[2]->str;
                payload = msg->element[3]->str;
            } else if (msgType == "message") {
                channel = msg->element[1]->str;
                payload = msg->element[2]->str;
            } else {
                freeReplyObject(msg);
                continue;
            }

            PubSubMessage pubMsg{currentTimestampMs(), std::move(channel), std::move(payload)};
            {
                std::lock_guard lock(messageMutex_);
                pendingMessages_.push_back(std::move(pubMsg));
                pendingCount_.fetch_add(1, std::memory_order_release);
            }
            totalMessageCount_.fetch_add(1, std::memory_order_relaxed);
        }

        freeReplyObject(msg);
    }
}

void RedisPubSubTab::unsubscribe() {
    if (subThread_.joinable()) {
        subThread_.request_stop();
        subThread_.join();
    }
    if (subContext_) {
        redisFree(subContext_);
        subContext_ = nullptr;
    }
    if (subSslCtx_) {
        redisFreeSSLContext(subSslCtx_);
        subSslCtx_ = nullptr;
    }
    if (subState_.load() != SubState::Error)
        subState_.store(SubState::Idle);
    activePattern_.clear();
}

void RedisPubSubTab::drainPendingMessages() {
    if (pendingCount_.load(std::memory_order_acquire) == 0)
        return;

    std::lock_guard lock(messageMutex_);

    // prepend newest messages at front
    displayMessages_.insert(displayMessages_.begin(),
                            std::make_move_iterator(pendingMessages_.rbegin()),
                            std::make_move_iterator(pendingMessages_.rend()));
    pendingMessages_.clear();
    pendingCount_.store(0, std::memory_order_relaxed);

    constexpr size_t maxDisplay = 10000;
    if (displayMessages_.size() > maxDisplay)
        displayMessages_.resize(maxDisplay);
}

void RedisPubSubTab::publish(const std::string& channel, const std::string& message) {
    if (!db_ || !db_->isConnected())
        return;
    // quote channel and message so spaces and special chars are preserved
    auto quote = [](const std::string& s) {
        std::string q = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\')
                q += '\\';
            q += c;
        }
        q += '"';
        return q;
    };
    std::string cmd = std::format("PUBLISH {} {}", quote(channel), quote(message));
    auto result = db_->executeQuery(cmd);
    if (!result.empty() && !result[0].success) {
        Logger::error(std::format("Pub/Sub publish failed: {}", result[0].errorMessage));
    }
}

std::string RedisPubSubTab::currentTimestampMs() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_r(&time, &tm);
    return std::format("{:02d}:{:02d}:{:02d}.{:03d}", tm.tm_hour, tm.tm_min, tm.tm_sec,
                       static_cast<int>(ms.count()));
}

void RedisPubSubTab::render() {
    drainPendingMessages();

    const auto& colors = Application::getInstance().getCurrentColors();
    constexpr float toggleStripWidth = 28.0f;
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    const float totalHeight = ImGui::GetContentRegionAvail().y;
    const float panelContentWidth = statusPanelOpen_ ? RedisStatusPanel::kFixedPanelWidth : 0.0f;
    float mainWidth = totalWidth - toggleStripWidth - panelContentWidth;
    mainWidth = std::max(220.0f, mainWidth);
    float statusTopOffset = 0.0f;
    float statusHeight = totalHeight;

    if (ImGui::BeginChild("##redis_pubsub_main", ImVec2(mainWidth, totalHeight), false)) {
        // header
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
        ImGui::Text(ICON_FA_TOWER_BROADCAST " Pub/Sub");
        ImGui::Separator();

        statusTopOffset = ImGui::GetCursorPosY();
        statusHeight = ImGui::GetContentRegionAvail().y;

        // border on all input fields
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.surface2);

        // main content area (between header and publish bar)
        const float publishBarHeight = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::M;
        const float contentHeight = ImGui::GetContentRegionAvail().y - publishBarHeight;

        ImGui::BeginChild("##pubsub_content", ImVec2(0, contentHeight));

        renderToolbar(colors);

        ImGui::Separator();

        renderMessageTable(colors);

        ImGui::EndChild();

        renderPublishBar(colors);

        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    const float alignedStatusTop =
        (statusTopOffset > Theme::Spacing::S) ? (statusTopOffset - Theme::Spacing::S) : 0.0f;
    const float statusTopDelta = statusTopOffset - alignedStatusTop;
    const float alignedStatusHeight =
        std::min(totalHeight - alignedStatusTop, statusHeight + statusTopDelta);

    if (statusPanelOpen_) {
        ImGui::SameLine(0, 0);
        if (ImGui::BeginChild("##redis_pubsub_status_wrap",
                              ImVec2(RedisStatusPanel::kFixedPanelWidth, totalHeight), false)) {
            ImGui::SetCursorPosY(alignedStatusTop);
            statusPanel_.renderPanel(alignedStatusHeight, "##redis_pubsub_status_panel");
        }
        ImGui::EndChild();
    }

    ImGui::SameLine(0, 0);
    if (ImGui::BeginChild("##redis_pubsub_status_strip_wrap", ImVec2(toggleStripWidth, totalHeight),
                          false)) {
        ImGui::SetCursorPosY(alignedStatusTop);
        RedisStatusPanel::renderToggleStrip(statusPanelOpen_, toggleStripWidth, alignedStatusHeight,
                                            "##redis_pubsub_status_strip",
                                            "##redis_pubsub_status_toggle");
    }
    ImGui::EndChild();
}

void RedisPubSubTab::renderToolbar(const Theme::Colors& colors) {
    auto state = subState_.load();
    bool subscribed = (state == SubState::Subscribed);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);
    ImGui::AlignTextToFramePadding();

    // stats
    ImGui::Text("Messages: %d", totalMessageCount_.load());
    ImGui::SameLine(0, Theme::Spacing::L);

    // pattern input (editable only when not subscribed)
    if (subscribed)
        ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##sub_pattern", "Channel pattern", patternBuf_, sizeof(patternBuf_));
    if (subscribed)
        ImGui::EndDisabled();

    ImGui::SameLine(0, Theme::Spacing::M);

    // subscribe / unsubscribe button
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    if (subscribed) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
        if (ImGui::Button(ICON_FA_CIRCLE_MINUS " Unsubscribe")) {
            unsubscribe();
            subState_.store(SubState::Idle);
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(colors.green.x, colors.green.y, colors.green.z, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.6f));
        bool busy = (state == SubState::Subscribing);
        if (busy)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_TOWER_BROADCAST " Subscribe")) {
            subscribe(patternBuf_);
        }
        if (busy)
            ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
    }
    ImGui::PopStyleColor(); // Border
    ImGui::PopStyleVar();

    ImGui::SameLine(0, Theme::Spacing::S);

    // clear button
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
    if (ImGui::Button(ICON_FA_TRASH_CAN " Clear")) {
        displayMessages_.clear();
        totalMessageCount_.store(0);
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    // error display
    const std::string subError = getSubError();
    if (state == SubState::Error && !subError.empty()) {
        ImGui::SameLine(0, Theme::Spacing::L);
        ImGui::TextColored(colors.red, "%s", subError.c_str());
    }
}

void RedisPubSubTab::renderMessageTable(const Theme::Colors& colors) {
    const float tableHeight = std::max(ImGui::GetContentRegionAvail().y, 50.0f);

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##pubsub_messages", 3, flags, ImVec2(-1, tableHeight))) {
        ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(displayMessages_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& msg = displayMessages_[row];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", msg.timestamp.c_str());

                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(colors.sky));
                ImGui::TextUnformatted(msg.channel.c_str());
                ImGui::PopStyleColor();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(msg.message.c_str());
            }
        }

        ImGui::EndTable();
    }
}

void RedisPubSubTab::renderPublishBar(const Theme::Colors& colors) {
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::XS);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Channel name");
    ImGui::SameLine(0, Theme::Spacing::M);

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##pub_channel", "channel", publishChannelBuf_,
                             sizeof(publishChannelBuf_));
    ImGui::SameLine(0, Theme::Spacing::L);

    ImGui::Text("Message");
    ImGui::SameLine(0, Theme::Spacing::M);

    const float buttonWidth = 80.0f;
    const float remainingWidth = ImGui::GetContentRegionAvail().x - buttonWidth - Theme::Spacing::L;
    if (refocusMessageInput_) {
        ImGui::SetKeyboardFocusHere();
        refocusMessageInput_ = false;
    }
    ImGui::SetNextItemWidth(std::max(remainingWidth, 100.0f));
    bool enterPressed =
        ImGui::InputTextWithHint("##pub_message", "Enter Message", publishMessageBuf_,
                                 sizeof(publishMessageBuf_), ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine(0, Theme::Spacing::L);

    bool canPublish = publishChannelBuf_[0] != '\0' && publishMessageBuf_[0] != '\0';
    if (!canPublish)
        ImGui::BeginDisabled();

    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
    if (ImGui::Button("Publish", ImVec2(buttonWidth, 0)) || (enterPressed && canPublish)) {
        publish(publishChannelBuf_, publishMessageBuf_);
        publishMessageBuf_[0] = '\0';
        refocusMessageInput_ = true;
    }
    ImGui::PopStyleColor(4);

    if (!canPublish)
        ImGui::EndDisabled();
}
