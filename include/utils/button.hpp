#pragma once

#include "imgui.h"
#include <initializer_list>

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
    // a row of small toggle buttons, one selected; true when the selection changed
    bool SegmentedTabs(const char* id, std::initializer_list<const char*> labels, int* current);
    // label button with a smaller icon in front of the text
    bool IconLabelButton(const char* icon, const char* label,
                         ButtonVariant variant = ButtonVariant::Secondary);
    bool IconButton(const char* icon, ButtonVariant variant = ButtonVariant::Secondary,
                    const ImVec2& size = ImVec2(0.0f, 0.0f),
                    ButtonSize buttonSize = ButtonSize::Default);
} // namespace UIUtils
