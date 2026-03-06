#include "utils/spinner.hpp"
#include "imgui_internal.h"

namespace UIUtils {
    bool Spinner(const char* label, float radius, int thickness, const ImU32& color) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID(label);

        const ImVec2 pos = window->DC.CursorPos;
        const ImVec2 size((radius) * 2, (radius + style.FramePadding.y) * 2);

        const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(bb, style.FramePadding.y);
        if (!ImGui::ItemAdd(bb, id))
            return false;

        // Render
        window->DrawList->PathClear();

        constexpr int num_segments = 30;
        const float start = abs(ImSin(g.Time * 1.8f) * (num_segments - 5));

        const float a_min = IM_PI * 2.0f * start / num_segments;
        constexpr float a_max = IM_PI * 2.0f * (num_segments - 3) / num_segments;

        const auto centre = ImVec2(pos.x + radius, pos.y + radius + style.FramePadding.y);

        for (int i = 0; i < num_segments; i++) {
            const float a = a_min + (static_cast<float>(i) / static_cast<float>(num_segments)) *
                                        (a_max - a_min);
            window->DrawList->PathLineTo(ImVec2(centre.x + ImCos(a + g.Time * 8) * radius,
                                                centre.y + ImSin(a + g.Time * 8) * radius));
        }

        window->DrawList->PathStroke(color, false, static_cast<float>(thickness));
        return true;
    }

    void SpinnerOverlay(ImDrawList* drawList, const ImVec2 centre, const float radius,
                        const int thickness, const ImU32 color) {
        if (!drawList) {
            return;
        }

        const float time = static_cast<float>(ImGui::GetTime());
        constexpr int numSegments = 30;
        const float start = abs(ImSin(time * 1.8f) * (numSegments - 5));
        const float aMin = IM_PI * 2.0f * start / numSegments;
        constexpr float aMax = IM_PI * 2.0f * (numSegments - 3) / numSegments;
        const float rotation = time * 8.0f;

        drawList->PathClear();
        for (int i = 0; i < numSegments; i++) {
            const float a =
                aMin + (static_cast<float>(i) / static_cast<float>(numSegments)) * (aMax - aMin);
            drawList->PathLineTo(ImVec2(centre.x + ImCos(a + rotation) * radius,
                                        centre.y + ImSin(a + rotation) * radius));
        }
        drawList->PathStroke(color, false, static_cast<float>(thickness));
    }
} // namespace UIUtils
