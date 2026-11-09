#pragma once

#include "imgui.h"
#include "themes.hpp"

#include <algorithm>

// every menu opens through these: a popup takes its padding when it opens,
// so the tighter padding is pushed only for that moment
namespace UIUtils {
    // dialogs: never larger than the screen
    inline ImVec2 DialogRoom() {
        const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
        return {std::max(work.x - 2 * Theme::Spacing::M, 1.0f),
                std::max(work.y - 2 * Theme::Spacing::M, 1.0f)};
    }
    inline ImVec2 FitScreen(const ImVec2 size) {
        const ImVec2 room = DialogRoom();
        return {size.x > 0 ? std::min(size.x, room.x) : size.x,
                size.y > 0 ? std::min(size.y, room.y) : size.y};
    }
    inline void CapToScreen() {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), DialogRoom());
    }

    // inner padding of every menu
    inline constexpr float kMenuPadding = Theme::Spacing::M + Theme::Spacing::S;

    inline bool BeginMenuPopup(const char* id, ImGuiWindowFlags flags = 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kMenuPadding, kMenuPadding));
        const bool open = ImGui::BeginPopup(id, flags);
        ImGui::PopStyleVar();
        return open;
    }

    inline bool BeginContextMenu(const char* id = nullptr,
                                 ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonRight) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kMenuPadding, kMenuPadding));
        const bool open = ImGui::BeginPopupContextItem(id, flags);
        ImGui::PopStyleVar();
        return open;
    }

    inline bool BeginSubMenu(const char* label, bool enabled = true) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kMenuPadding, kMenuPadding));
        const bool open = ImGui::BeginMenu(label, enabled);
        ImGui::PopStyleVar();
        return open;
    }

    // dropdowns: same padding and item spacing as the menus. close with EndSelect()
    inline bool BeginSelect(const char* id, const char* preview, ImGuiComboFlags flags = 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kMenuPadding, kMenuPadding));
        const bool open = ImGui::BeginCombo(id, preview, flags);
        ImGui::PopStyleVar();
        if (open)
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        return open;
    }

    inline void EndSelect() {
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }
} // namespace UIUtils
