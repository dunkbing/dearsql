#pragma once

#include "imgui.h"

namespace UIUtils {
    enum class ButtonVariant {
        Secondary,
        Primary,
        Danger,
    };

    bool Button(const char* label, ButtonVariant variant = ButtonVariant::Secondary,
                const ImVec2& size = ImVec2(0.0f, 0.0f));
    bool SmallButton(const char* label, ButtonVariant variant = ButtonVariant::Secondary);
} // namespace UIUtils
