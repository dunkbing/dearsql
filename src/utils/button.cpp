#include "utils/button.hpp"

#include "application.hpp"
#include "imgui_internal.h"
#include "themes.hpp"

namespace {
    void pushVariantColors(UIUtils::ButtonVariant variant) {
        if (variant == UIUtils::ButtonVariant::Custom) {
            return;
        }

        const auto& colors = Application::getInstance().getCurrentColors();
        if (variant == UIUtils::ButtonVariant::Secondary) {
            ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
            return;
        }

        const ImVec4 accent =
            variant == UIUtils::ButtonVariant::Primary ? colors.green : colors.red;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
    }

    void popVariantColors(UIUtils::ButtonVariant variant) {
        if (variant == UIUtils::ButtonVariant::Secondary) {
            ImGui::PopStyleColor(3);
        } else if (variant != UIUtils::ButtonVariant::Custom) {
            ImGui::PopStyleColor(4);
        }
    }

    void pushIconColors(UIUtils::ButtonVariant variant) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImVec4 hovered = colors.surface1;
        ImVec4 active = colors.surface2;

        if (variant == UIUtils::ButtonVariant::Primary ||
            variant == UIUtils::ButtonVariant::Danger) {
            const ImVec4 accent =
                variant == UIUtils::ButtonVariant::Primary ? colors.green : colors.red;
            hovered = ImVec4(accent.x, accent.y, accent.z, 0.25f);
            active = ImVec4(accent.x, accent.y, accent.z, 0.4f);
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    }

    void drawButtonShadow(const char* label, const ImVec2& size, const ImVec2& padding) {
        if (ImGui::GetStyleColorVec4(ImGuiCol_Button).w <= 0.0f) {
            return;
        }

        const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
        const ImVec2 buttonSize =
            ImGui::CalcItemSize(size, textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        const float baseAlpha = Application::getInstance().isDarkTheme() ? 0.28f : 0.16f;
        constexpr int layers = 5;

        for (int layer = layers; layer >= 0; --layer) {
            const float spread = static_cast<float>(layer) * 0.8f;
            const float strength =
                1.0f - static_cast<float>(layer) / static_cast<float>(layers + 1);
            const float alpha = baseAlpha * strength * 0.22f;
            const ImVec2 min(cursorPos.x - spread, cursorPos.y + 2.0f - spread);
            const ImVec2 max(cursorPos.x + buttonSize.x + spread,
                             cursorPos.y + buttonSize.y + 2.0f + spread);

            ImGui::GetWindowDrawList()->AddRectFilled(
                min, max, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, alpha)),
                Theme::CornerRadius::SMALL + spread);
        }
    }
} // namespace

bool UIUtils::Button(const char* label, ButtonVariant variant, const ImVec2& size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::CornerRadius::SMALL);
    pushVariantColors(variant);
    drawButtonShadow(label, size, ImGui::GetStyle().FramePadding);
    const bool clicked = ImGui::Button(label, size);
    popVariantColors(variant);
    ImGui::PopStyleVar(2);
    return clicked;
}

bool UIUtils::SmallButton(const char* label, ButtonVariant variant) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::CornerRadius::SMALL);
    pushVariantColors(variant);
    drawButtonShadow(label, ImVec2(0.0f, 0.0f), ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    const bool clicked = ImGui::SmallButton(label);
    popVariantColors(variant);
    ImGui::PopStyleVar(2);
    return clicked;
}

bool UIUtils::IconButton(const char* icon, ButtonVariant variant, const ImVec2& size,
                         ButtonSize buttonSize) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::CornerRadius::SMALL);
    pushIconColors(variant);
    const bool clicked =
        buttonSize == ButtonSize::Small ? ImGui::SmallButton(icon) : ImGui::Button(icon, size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return clicked;
}
