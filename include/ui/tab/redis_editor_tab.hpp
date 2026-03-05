#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/redis_status_panel.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <string>
#include <vector>

class RedisDatabase;

struct RedisResultEntry {
    std::string command;
    std::string result;
    bool success = true;
    std::string errorMessage;
    double durationMs = 0.0;
    std::string timestamp;
};

class RedisEditorTab final : public Tab {
public:
    explicit RedisEditorTab(const std::string& name, RedisDatabase* db);
    ~RedisEditorTab() override;

    void render() override;

private:
    RedisDatabase* db_;
    dearsql::TextEditor editor_;
    std::string command_;

    std::vector<RedisResultEntry> resultHistory_;
    AsyncOperation<std::vector<RedisResultEntry>> queryOp_;

    float splitterPosition_ = 0.35f;
    bool autoClearEditor_ = true;
    float totalContentHeight_ = 0.0f;
    int pendingFocusFrames_ = 3;
    bool statusPanelOpen_ = false;
    RedisStatusPanel statusPanel_;

    void startCommandExecutionAsync(const std::string& cmd);
    void checkCommandExecutionStatus();
    void renderHeader() const;
    void renderToolbar();
    void renderResults();
};
