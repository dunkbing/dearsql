#pragma once

#include "imgui.h"

namespace UIUtils {
    enum class ButtonVariant {
        Secondary,
        Primary,
        Danger,
        Custom,
    };

    enum class ButtonSize {
        Default,
        Small,
    };

    bool Button(const char* label, ButtonVariant variant = ButtonVariant::Secondary,
                const ImVec2& size = ImVec2(0.0f, 0.0f));
    bool SmallButton(const char* label, ButtonVariant variant = ButtonVariant::Secondary);
    bool IconButton(const char* icon, ButtonVariant variant = ButtonVariant::Secondary,
                    const ImVec2& size = ImVec2(0.0f, 0.0f),
                    ButtonSize buttonSize = ButtonSize::Default);
} // namespace UIUtils
