#pragma once

#include "imgui.h"
#include "themes.hpp"

// every menu opens through these: a popup takes its padding when it opens,
// so the tighter padding is pushed only for that moment
namespace UIUtils {
    inline bool BeginMenuPopup(const char* id, ImGuiWindowFlags flags = 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool open = ImGui::BeginPopup(id, flags);
        ImGui::PopStyleVar();
        return open;
    }

    inline bool BeginContextMenu(const char* id = nullptr,
                                 ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonRight) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool open = ImGui::BeginPopupContextItem(id, flags);
        ImGui::PopStyleVar();
        return open;
    }

    inline bool BeginSubMenu(const char* label, bool enabled = true) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool open = ImGui::BeginMenu(label, enabled);
        ImGui::PopStyleVar();
        return open;
    }
} // namespace UIUtils
