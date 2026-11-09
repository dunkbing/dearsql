#pragma once

#include <functional>
#include <string>
#include <vector>

struct AlertButton {
    std::string text;
    std::function<void()> onPress;
    enum class Style { Default, Cancel, Destructive };
    Style style = Style::Default;
};

// an imgui modal over everything, one at a time; show() queues it (any thread) and returns
class Alert {
public:
    static void show(const std::string& title, const std::string& message = "",
                     std::vector<AlertButton> buttons = {});
    // the queued alert or input prompt: once a frame, after the rest of the ui
    static void render();
};
