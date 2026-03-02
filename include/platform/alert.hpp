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

class Alert {
public:
    static void show(const std::string& title, const std::string& message = "",
                     std::vector<AlertButton> buttons = {});
};
