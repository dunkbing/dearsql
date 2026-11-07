#include "utils/select.hpp"

#include "application.hpp"
#include "imgui.h"
#include "utils/menu.hpp"

bool UIUtils::Select(const char* label, int* currentItem, const char* const items[],
                     int itemCount) {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
    bool changed = false;
    const char* preview = *currentItem >= 0 && *currentItem < itemCount ? items[*currentItem] : "";
    if (BeginSelect(label, preview)) {
        for (int i = 0; i < itemCount; ++i) {
            const bool selected = i == *currentItem;
            if (ImGui::Selectable(items[i], selected) && !selected) {
                *currentItem = i;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        EndSelect();
    }
    ImGui::PopStyleColor(3);
    return changed;
}
