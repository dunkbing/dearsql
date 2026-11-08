#pragma once

#include "imgui.h"
#include <algorithm>

namespace UIUtils {

    // invisible drag handle right above the lower pane: its top border is what you grab,
    // lit while hovered or dragged
    // returns true if position changed
    inline bool Splitter(const char* id, float* position, float availableHeight, float minSize1,
                         float minSize2) {
        constexpr float hoverThickness = 4.0f;

        ImGui::InvisibleButton(id, ImVec2(-1, hoverThickness));

        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();

        if (hovered || held) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }

        bool changed = false;
        if (held) {
            const float delta = ImGui::GetIO().MouseDelta.y;
            if (delta != 0.0f) {
                const float currentPixelPos = *position * availableHeight;
                const float newPixelPos = currentPixelPos + delta;
                float newPosition = newPixelPos / availableHeight;

                const float minPos = minSize1 / availableHeight;
                const float maxPos = 1.0f - (minSize2 / availableHeight);
                newPosition = std::max(minPos, std::min(maxPos, newPosition));

                if (newPosition != *position) {
                    *position = newPosition;
                    changed = true;
                }
            }
        }

        if (hovered || held) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(min.x, max.y - 1.0f), ImVec2(max.x, max.y - 1.0f),
                ImGui::GetColorU32(ImGuiCol_SeparatorHovered), 2.0f);
        }

        return changed;
    }

} // namespace UIUtils
