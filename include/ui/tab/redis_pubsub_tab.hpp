#pragma once

#include "themes.hpp"
#include "ui/tab/redis_status_panel.hpp"
#include "ui/tab/tab.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct redisContext;
struct redisSSLContext;
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
    std::atomic<SubState> subState_{SubState::Idle};
    mutable std::mutex subErrorMutex_;
    std::string subError_;

    char patternBuf_[256] = "*";
    std::string activePattern_;

    char publishChannelBuf_[256] = {};
    char publishMessageBuf_[4096] = {};
    bool refocusMessageInput_ = false;

    // subscriber connection (separate from main db context)
    redisContext* subContext_ = nullptr;
    redisSSLContext* subSslCtx_ = nullptr;
    std::jthread subThread_;

    // thread-safe message queue
    std::mutex messageMutex_;
    std::vector<PubSubMessage> pendingMessages_;
    std::vector<PubSubMessage> displayMessages_;
    std::atomic<int> totalMessageCount_{0};
    std::atomic<int> pendingCount_{0};
    bool statusPanelOpen_ = false;
    RedisStatusPanel statusPanel_;

    void subscribe(const std::string& pattern);
    void unsubscribe();
    void subscriberLoop(std::stop_token stopToken);
    redisContext* createSubscriberContext();
    void setSubError(std::string error);
    void clearSubError();
    [[nodiscard]] std::string getSubError() const;

    void renderToolbar(const Theme::Colors& colors);
    void renderMessageTable(const Theme::Colors& colors);
    void renderPublishBar(const Theme::Colors& colors);
    void drainPendingMessages();
    void publish(const std::string& channel, const std::string& message);

    static std::string currentTimestampMs();
};
