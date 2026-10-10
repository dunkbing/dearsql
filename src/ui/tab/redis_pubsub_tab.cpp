#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/tab/redis_pubsub_tab.hpp"
#include "utils/button.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <hiredis/hiredis.h>
#include <hiredis/hiredis_ssl.h>
#include <spdlog/spdlog.h>

namespace {
    void shutdownSocket(std::intptr_t fd) {
        if (fd < 0)
            return;
#ifdef _WIN32
        shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
        shutdown(static_cast<int>(fd), SHUT_RDWR);
#endif
    }
} // namespace

RedisPubSubTab::RedisPubSubTab(const std::string& name, RedisDatabase* db)
    : Tab(name, TabType::REDIS_PUBSUB), db_(db), statusPanel_(db) {
    strncpy(publishChannelBuf_, "*", sizeof(publishChannelBuf_) - 1);
}

RedisPubSubTab::~RedisPubSubTab() {
    unsubscribe();
}

// runs on the worker and owns the context; everything it reports goes through sub
void RedisPubSubTab::runSubscriber(const std::shared_ptr<Subscription>& sub,
                                   const DatabaseConnectionInfo& info, const std::string& pattern,
                                   const std::stop_token& stop) {
    redisSSLContext* sslCtx = nullptr;
    redisContext* ctx = nullptr;
    auto fail = [&](std::string error) {
        {
            std::lock_guard lock(sub->mutex);
            sub->error = std::move(error);
        }
        sub->state.store(SubState::Error);
    };
    // unpublish the socket before it is freed, so a stop never shuts down a reused fd
    auto cleanup = [&] {
        {
            std::lock_guard lock(sub->mutex);
            sub->fd = -1;
        }
        if (ctx)
            redisFree(ctx);
        if (sslCtx)
            redisFreeSSLContext(sslCtx);
    };

    constexpr timeval timeout = {5, 0};
    ctx = redisConnectWithTimeout(info.host.c_str(), info.port, timeout);
    if (!ctx || ctx->err) {
        fail(ctx ? ctx->errstr : "Failed to allocate subscriber context");
        cleanup();
        return;
    }
    {
        std::lock_guard lock(sub->mutex);
        sub->fd = static_cast<std::intptr_t>(ctx->fd);
    }
    // stopped while connecting: the shutdown missed the socket
    if (stop.stop_requested()) {
        cleanup();
        return;
    }
    // no handshake step (TLS, AUTH, SUBSCRIBE) may block for good on a stalled server
    redisSetTimeout(ctx, timeout);

    if (info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
        info.sslmode == SslMode::VerifyFull) {
        redisSSLContextError sslErr = REDIS_SSL_CTX_NONE;
        const char* caPath = (!info.sslCACertPath.empty() && (info.sslmode == SslMode::VerifyCA ||
                                                              info.sslmode == SslMode::VerifyFull))
                                 ? info.sslCACertPath.c_str()
                                 : nullptr;
        sslCtx = redisCreateSSLContext(caPath, nullptr, nullptr, nullptr, nullptr, &sslErr);
        if (!sslCtx) {
            fail(std::string("Subscriber TLS context failed: ") + redisSSLContextGetError(sslErr));
            cleanup();
            return;
        }
        if (redisInitiateSSLWithContext(ctx, sslCtx) != REDIS_OK) {
            fail(ctx->errstr[0] ? ctx->errstr : "Subscriber TLS handshake failed");
            cleanup();
            return;
        }
    }

    if (!info.password.empty()) {
        redisReply* reply = nullptr;
        if (!info.username.empty()) {
            reply = static_cast<redisReply*>(
                redisCommand(ctx, "AUTH %s %s", info.username.c_str(), info.password.c_str()));
        } else {
            reply = static_cast<redisReply*>(redisCommand(ctx, "AUTH %s", info.password.c_str()));
        }
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            if (!stop.stop_requested())
                fail(reply ? reply->str : "Subscriber auth failed");
            if (reply)
                freeReplyObject(reply);
            cleanup();
            return;
        }
        freeReplyObject(reply);
    }

    // PSUBSCRIBE for glob patterns, SUBSCRIBE for an exact channel
    const bool isPattern = pattern.find_first_of("*?[") != std::string::npos;
    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx, "%s %s", isPattern ? "PSUBSCRIBE" : "SUBSCRIBE", pattern.c_str()));
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        if (!stop.stop_requested())
            fail(reply ? reply->str : (ctx->errstr[0] ? ctx->errstr : "Subscribe failed"));
        if (reply)
            freeReplyObject(reply);
        cleanup();
        return;
    }
    freeReplyObject(reply);
    sub->state.store(SubState::Subscribed);

    // wait on poll (100ms) so a stop is seen; read only once data is there
#ifdef _WIN32
    WSAPOLLFD pfd = {};
    pfd.fd = ctx->fd;
    pfd.events = POLLIN;
#else
    struct pollfd pfd = {.fd = ctx->fd, .events = POLLIN, .revents = 0};
#endif

    while (!stop.stop_requested()) {
        const int pollRc =
#ifdef _WIN32
            WSAPoll(&pfd, 1, 100);
#else
            poll(&pfd, 1, 100);
#endif
        if (pollRc == 0)
            continue;
        if (pollRc < 0) {
            if (errno == EINTR)
                continue;
            fail("poll error: " + std::string(strerror(errno)));
            break;
        }
        if (stop.stop_requested())
            break;

        redisReply* msg = nullptr;
        if (redisGetReply(ctx, reinterpret_cast<void**>(&msg)) == REDIS_ERR) {
            if (!stop.stop_requested())
                fail(ctx->errstr);
            break;
        }
        if (!msg)
            continue;

        if (msg->type == REDIS_REPLY_ARRAY && msg->elements >= 3) {
            const std::string msgType = msg->element[0]->str ? msg->element[0]->str : "";
            std::string channel;
            std::string payload;
            bool isMessage = true;
            if (msgType == "pmessage" && msg->elements >= 4) {
                channel = msg->element[2]->str ? msg->element[2]->str : "";
                payload = msg->element[3]->str ? msg->element[3]->str : "";
            } else if (msgType == "message") {
                channel = msg->element[1]->str ? msg->element[1]->str : "";
                payload = msg->element[2]->str ? msg->element[2]->str : "";
            } else {
                isMessage = false;
            }
            if (isMessage) {
                PubSubMessage pubMsg{currentTimestampMs(), std::move(channel), std::move(payload)};
                {
                    std::lock_guard lock(sub->mutex);
                    sub->pending.push_back(std::move(pubMsg));
                    sub->pendingCount.fetch_add(1, std::memory_order_release);
                }
                sub->total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        freeReplyObject(msg);
    }
    cleanup();
}

void RedisPubSubTab::subscribe(const std::string& pattern) {
    auto cur = sub_->state.load();
    if (cur == SubState::Subscribed || cur == SubState::Subscribing)
        return;
    if (!db_)
        return;

    // a previous subscription's worker may still be unwinding: it keeps its own state
    unsubscribe();
    displayMessages_.clear();
    sub_ = std::make_shared<Subscription>();
    sub_->state.store(SubState::Subscribing);
    activePattern_ = pattern;

    // connect, TLS and AUTH happen on the worker: a slow server must not freeze the ui
    subOp_.setWakesFrames(false);
    subOp_.check();
    subOp_.startCancellable(
        [sub = sub_, info = db_->getConnectionInfo(), pattern](std::stop_token st) {
            runSubscriber(sub, info, pattern, st);
            return true;
        });
}

void RedisPubSubTab::unsubscribe() {
    // stop the worker without waiting: break a blocking read by shutting the
    // socket down, then let it unwind and free the context on its own
    subOp_.cancel();
    {
        std::lock_guard lock(sub_->mutex);
        shutdownSocket(sub_->fd);
    }
    subOp_.detach();

    // the old worker keeps writing into its own state; the tab moves on
    auto next = std::make_shared<Subscription>();
    if (sub_->state.load() == SubState::Error) {
        std::lock_guard lock(sub_->mutex);
        next->error = sub_->error;
        next->state.store(SubState::Error);
    }
    next->total.store(sub_->total.load());
    {
        std::lock_guard lock(sub_->mutex);
        next->pending = std::move(sub_->pending);
        next->pendingCount.store(static_cast<int>(next->pending.size()));
    }
    sub_ = std::move(next);
    activePattern_.clear();
}

void RedisPubSubTab::drainPendingMessages() {
    if (sub_->pendingCount.load(std::memory_order_acquire) == 0)
        return;

    std::lock_guard lock(sub_->mutex);

    // prepend newest messages at front
    displayMessages_.insert(displayMessages_.begin(),
                            std::make_move_iterator(sub_->pending.rbegin()),
                            std::make_move_iterator(sub_->pending.rend()));
    sub_->pending.clear();
    sub_->pendingCount.store(0, std::memory_order_relaxed);

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
        spdlog::error("Pub/Sub publish failed: {}", result[0].errorMessage);
    }
}

std::string RedisPubSubTab::currentTimestampMs() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
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

        // stronger border on all input fields
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
    auto state = sub_->state.load();
    bool subscribed = (state == SubState::Subscribed);
    const bool connecting = (state == SubState::Subscribing);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Theme::Spacing::S);
    ImGui::AlignTextToFramePadding();

    // stats
    ImGui::Text("Messages: %d", sub_->total.load());
    ImGui::SameLine(0, Theme::Spacing::L);

    // pattern input (editable only when not subscribed)
    ImGui::BeginDisabled(subscribed || connecting);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##sub_pattern", "Channel pattern", patternBuf_, sizeof(patternBuf_));
    ImGui::EndDisabled();

    ImGui::SameLine(0, Theme::Spacing::M);

    // subscribe / unsubscribe button
    if (subscribed) {
        if (UIUtils::Button(ICON_FA_CIRCLE_MINUS " Unsubscribe", UIUtils::ButtonVariant::Danger)) {
            unsubscribe();
        }
    } else if (connecting) {
        // connecting runs on a worker: it can be called off
        if (UIUtils::Button(ICON_FA_XMARK " Cancel", UIUtils::ButtonVariant::Secondary))
            unsubscribe();
    } else {
        if (UIUtils::Button(ICON_FA_TOWER_BROADCAST " Subscribe",
                            UIUtils::ButtonVariant::Primary)) {
            subscribe(patternBuf_);
        }
    }

    ImGui::SameLine(0, Theme::Spacing::S);

    // clear button
    if (UIUtils::Button(ICON_FA_TRASH_CAN " Clear", UIUtils::ButtonVariant::Danger)) {
        displayMessages_.clear();
        sub_->total.store(0);
    }

    // error display
    std::string subError;
    {
        std::lock_guard lock(sub_->mutex);
        subError = sub_->error;
    }
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
    ImGui::SameLine(0, Theme::Spacing::XL * 1.5);

    ImGui::Text("Message");
    ImGui::SameLine(0, Theme::Spacing::M);

    const float buttonWidth = 80.0f;
    const float remainingWidth = ImGui::GetContentRegionAvail().x - buttonWidth;
    if (refocusMessageInput_) {
        ImGui::SetKeyboardFocusHere();
        refocusMessageInput_ = false;
    }
    ImGui::SetNextItemWidth(std::max(remainingWidth, 100.0f));
    bool enterPressed =
        ImGui::InputTextWithHint("##pub_message", "Enter Message", publishMessageBuf_,
                                 sizeof(publishMessageBuf_), ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine(0, 0);

    bool canPublish = publishChannelBuf_[0] != '\0' && publishMessageBuf_[0] != '\0';
    if (!canPublish)
        ImGui::BeginDisabled();

    if (UIUtils::Button("Publish", UIUtils::ButtonVariant::Primary, ImVec2(buttonWidth, 0)) ||
        (enterPressed && canPublish)) {
        publish(publishChannelBuf_, publishMessageBuf_);
        publishMessageBuf_[0] = '\0';
        refocusMessageInput_ = true;
    }

    if (!canPublish)
        ImGui::EndDisabled();
}
