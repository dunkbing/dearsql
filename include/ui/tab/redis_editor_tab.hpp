#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <chrono>
#include <string>

class RedisDatabase;

class RedisEditorTab final : public Tab {
public:
    explicit RedisEditorTab(const std::string& name, RedisDatabase* db);
    ~RedisEditorTab() override;

    void render() override;

private:
    RedisDatabase* db_;
    dearsql::TextEditor editor_;
    std::string command_;

    QueryResult queryResult_;
    std::string queryError_;
    std::chrono::milliseconds lastQueryDuration_{0};

    AsyncOperation<QueryResult> queryOp_;

    float splitterPosition_ = 0.35f;
    bool splitterActive_ = false;
    float totalContentHeight_ = 0.0f;
    int pendingFocusFrames_ = 3;

    void startCommandExecutionAsync(const std::string& cmd);
    void checkCommandExecutionStatus();
    void renderHeader() const;
    void renderToolbar();
    void renderResults() const;
    bool renderVerticalSplitter(const char* id, float* position, float minSize1,
                                float minSize2) const;
};
