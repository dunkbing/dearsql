#include "utils/toggle_button.hpp"
#include "application.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "themes.hpp"

void UIUtils::ToggleButton(const char *str_id, bool *v) {
    const auto &colors =
        Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();

    const float height = ImGui::GetFrameHeight();
    const float width = height * 1.55f;
    const float radius = height * 0.50f;

    ImGui::InvisibleButton(str_id, ImVec2(width, height));
    if (ImGui::IsItemClicked())
        *v = !*v;

    ImGuiContext &gg = *GImGui;
    if (gg.LastActiveId == gg.CurrentWindow->GetID(str_id)) {
        if (ImGui::IsItemHovered()) {
            draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                                     ImGui::GetColorU32(*v ? colors.blue : colors.overlay1),
                                     height * 0.5f);
        } else {
            draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                                     ImGui::GetColorU32(*v ? colors.sapphire : colors.overlay0),
                                     height * 0.50f);
        }
    }

    draw_list->AddCircleFilled(
        ImVec2(p.x + radius + (*v ? 1.0f : 0.0f) * (width - radius * 2.0f), p.y + radius),
        radius - 1.5f, ImGui::GetColorU32(colors.text));
}
