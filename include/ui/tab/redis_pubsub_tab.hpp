#pragma once

#include "database/async_helper.hpp"
#include "database/db_interface.hpp"
#include "themes.hpp"
#include "ui/tab/redis_status_panel.hpp"
#include "ui/tab/tab.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RedisDatabase;

struct PubSubMessage {
    std::string timestamp;
    std::string channel;
    std::string message;
};

class RedisPubSubTab final : public Tab {
public:
    explicit RedisPubSubTab(const std::string& name, RedisDatabase* db);
    ~RedisPubSubTab() override;

    void render() override;

    [[nodiscard]] const RedisDatabase* getDatabase() const {
        return db_;
    }

private:
    RedisDatabase* db_;

    enum class SubState { Idle, Subscribing, Subscribed, Error };

    // what one subscription shares with its worker. the worker holds its own
    // reference and owns the hiredis context, so stopping never waits for it: the
    // socket is shut down under it and the worker is left to unwind
    struct Subscription {
        std::atomic<SubState> state{SubState::Idle};
        std::mutex mutex; // error, pending, fd
        std::string error;
        std::vector<PubSubMessage> pending;
        std::atomic<int> pendingCount{0};
        std::atomic<int> total{0};
        std::intptr_t fd = -1; // the open socket, -1 when none
    };
    std::shared_ptr<Subscription> sub_ = std::make_shared<Subscription>();

    char patternBuf_[256] = "*";
    std::string activePattern_;

    char publishChannelBuf_[256] = {};
    char publishMessageBuf_[4096] = {};
    bool refocusMessageInput_ = false;

    // subscriber connection (separate from main db context), run on a worker
    AsyncOperation<bool> subOp_;
    std::vector<PubSubMessage> displayMessages_;
    bool statusPanelOpen_ = false;
    RedisStatusPanel statusPanel_;

    void subscribe(const std::string& pattern);
    void unsubscribe();
    static void runSubscriber(const std::shared_ptr<Subscription>& sub,
                              const DatabaseConnectionInfo& info, const std::string& pattern,
                              const std::stop_token& stop);

    void renderToolbar(const Theme::Colors& colors);
    void renderMessageTable(const Theme::Colors& colors);
    void renderPublishBar(const Theme::Colors& colors);
    void drainPendingMessages();
    void publish(const std::string& channel, const std::string& message);

    static std::string currentTimestampMs();
};
