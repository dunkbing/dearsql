#pragma once

#include "imshell/themes.hpp"

#include <optional>
#include <string_view>

// Theme::Spacing and Theme::Radius keep working
using Theme = imshell::Theme;

// what the app keeps around imshell's themes: the saved pick and the connection colours
namespace themes {
    // a saved theme id; "light" and "dark" are what older versions stored, an unknown id (a
    // synced setting from a newer build) is the first preset
    inline const Theme& byId(std::string_view id) {
        if (id == "light")
            id = "one-light";
        else if (id == "dark")
            id = "one-dark";
        for (const Theme& t : imshell::Themes())
            if (id == t.id)
                return t;
        return imshell::Themes()[0];
    }

    // a connection stores a key, not a colour, so the pick reads in every theme; the keys are
    // saved (and synced), never rename them
    struct ConnectionColor {
        const char* key;
        ImVec4 Theme::Hues::* hue;
    };
    inline constexpr ConnectionColor kConnectionColors[] = {
        {"red", &Theme::Hues::red},     {"peach", &Theme::Hues::orange},
        {"yellow", &Theme::Hues::yellow}, {"green", &Theme::Hues::green},
        {"blue", &Theme::Hues::blue},   {"purple", &Theme::Hues::purple},
    };

    // none for an empty or unknown key
    inline std::optional<ImVec4> connectionColor(std::string_view key) {
        for (const auto& c : kConnectionColors)
            if (key == c.key)
                return imshell::CurrentTheme().hues.*c.hue;
        return std::nullopt;
    }

    // the table viewer's pagination strip; tabs without a bottom bar leave it free too
    inline float bottomBarHeight() {
        return ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    }
} // namespace themes
