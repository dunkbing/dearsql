#pragma once

#include "imgui.h"
#include <algorithm>

namespace UIUtils {

    // horizontal drag handle that controls a vertical split ratio
    // returns true if position changed
    inline bool Splitter(const char* id, float* position, float availableHeight, float minSize1,
                         float minSize2) {
        constexpr float hoverThickness = 6.0f;

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

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetItemRectMin();
        const ImVec2 size = ImGui::GetItemRectSize();
        const float centerY = pos.y + size.y / 2.0f;
        const ImU32 color = (hovered || held) ? ImGui::GetColorU32(ImGuiCol_SeparatorHovered)
                                              : ImGui::GetColorU32(ImGuiCol_Separator);
        drawList->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + size.x, centerY), color, 2.0f);

        return changed;
    }

} // namespace UIUtils
