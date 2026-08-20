#include "utils/button.hpp"

#include "application.hpp"
#include "themes.hpp"

namespace {
    void pushVariantColors(UIUtils::ButtonVariant variant) {
        if (variant == UIUtils::ButtonVariant::Secondary) {
            return;
        }

        const auto& colors = Application::getInstance().getCurrentColors();
        const ImVec4 accent =
            variant == UIUtils::ButtonVariant::Primary ? colors.green : colors.red;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
    }

    void popVariantColors(UIUtils::ButtonVariant variant) {
        if (variant != UIUtils::ButtonVariant::Secondary) {
            ImGui::PopStyleColor(4);
        }
    }
} // namespace

bool UIUtils::Button(const char* label, ButtonVariant variant, const ImVec2& size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::CornerRadius::SMALL);
    pushVariantColors(variant);
    const bool clicked = ImGui::Button(label, size);
    popVariantColors(variant);
    ImGui::PopStyleVar();
    return clicked;
}

bool UIUtils::SmallButton(const char* label, ButtonVariant variant) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::CornerRadius::SMALL);
    pushVariantColors(variant);
    const bool clicked = ImGui::SmallButton(label);
    popVariantColors(variant);
    ImGui::PopStyleVar();
    return clicked;
}
