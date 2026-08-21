#include "utils/select.hpp"

#include "application.hpp"
#include "imgui.h"

bool UIUtils::Select(const char* label, int* currentItem, const char* const items[],
                     int itemCount) {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
    const bool changed = ImGui::Combo(label, currentItem, items, itemCount);
    ImGui::PopStyleColor(3);
    return changed;
}
