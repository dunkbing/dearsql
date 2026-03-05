#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include <chrono>
#include <string>

class RedisDatabase;

struct RedisServerStatusSnapshot {
    std::string cpu = "-";
    std::string memory = "-";
    std::string totalKeys = "-";
    std::string connectedClients = "-";
    std::string lastUpdated = "-";
    std::string error;
    bool ready = false;
};

class RedisStatusPanel {
public:
    static constexpr float kFixedPanelWidth = 290.0f;

    explicit RedisStatusPanel(RedisDatabase* db = nullptr);

    void setDatabase(RedisDatabase* db);
    void tick();

    void renderPanel(float availableHeight, const char* panelId);

    static void renderToggleStrip(bool& panelOpen, float stripWidth, float availableHeight,
                                  const char* childId, const char* buttonId,
                                  const char* label = "Status");

private:
    RedisDatabase* db_ = nullptr;
    AsyncOperation<RedisServerStatusSnapshot> loadOp_;
    RedisServerStatusSnapshot snapshot_;
    bool loading_ = false;
    std::chrono::steady_clock::time_point lastRefreshAt_{};

    static RedisServerStatusSnapshot fetchStatus(RedisDatabase* db);
    static std::string extractInfoValue(const std::string& info, const std::string& key);
    static std::string readFirstCell(const QueryResult& result);
    static std::string formatNow();
};
