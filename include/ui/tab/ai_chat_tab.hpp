#pragma once

#include "ui/ai_sidebar_panel.hpp"
#include "ui/tab/tab.hpp"
#include <memory>
#include <string>

// assistant chat for one connection, opened from the sidebar
class AIChatTab : public Tab {
public:
    AIChatTab(const std::string& name, std::shared_ptr<DatabaseInterface> db)
        : Tab(name, TabType::AI_CHAT), panel_(std::move(db)) {}

    void render() override {
        panel_.render();
    }
    // the agent keeps running while the tab is hidden
    void tick() {
        panel_.tick();
    }
    AISidebarPanel& panel() {
        return panel_;
    }

private:
    AISidebarPanel panel_;
};
