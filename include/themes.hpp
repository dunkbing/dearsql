#pragma once
#include "imgui.h"

namespace Theme {
    struct Colors {
        ImVec4 rosewater;
        ImVec4 flamingo;
        ImVec4 pink;
        ImVec4 mauve;
        ImVec4 red;
        ImVec4 maroon;
        ImVec4 peach;
        ImVec4 yellow;
        ImVec4 green;
        ImVec4 teal;
        ImVec4 sky;
        ImVec4 sapphire;
        ImVec4 blue;
        ImVec4 lavender;
        ImVec4 text;
        ImVec4 subtext1;
        ImVec4 subtext0;
        ImVec4 overlay2;
        ImVec4 overlay1;
        ImVec4 overlay0;
        ImVec4 surface2;
        ImVec4 surface1;
        ImVec4 surface0;
        ImVec4 base;
        ImVec4 mantle;
        ImVec4 crust;
    };

    // Native Dark Theme (inspired by macOS/Windows dark modes)
    const Colors NATIVE_DARK = {
        ImVec4(0.95f, 0.85f, 0.85f, 1.0f), // rosewater
        ImVec4(0.93f, 0.79f, 0.81f, 1.0f), // flamingo
        ImVec4(0.96f, 0.76f, 0.82f, 1.0f), // pink
        ImVec4(0.91f, 0.76f, 0.96f, 1.0f), // mauve
        ImVec4(0.94f, 0.67f, 0.66f, 1.0f), // red
        ImVec4(0.94f, 0.68f, 0.74f, 1.0f), // maroon
        ImVec4(0.96f, 0.79f, 0.69f, 1.0f), // peach
        ImVec4(0.96f, 0.89f, 0.69f, 1.0f), // yellow
        ImVec4(0.65f, 0.90f, 0.67f, 1.0f), // green
        ImVec4(0.65f, 0.90f, 0.81f, 1.0f), // teal
        ImVec4(0.40f, 0.72f, 1.00f, 1.0f), // sky - system blue
        ImVec4(0.51f, 0.84f, 0.96f, 1.0f), // sapphire
        ImVec4(0.00f, 0.48f, 1.00f, 1.0f), // blue - system accent
        ImVec4(0.74f, 0.78f, 0.96f, 1.0f), // lavender
        ImVec4(1.00f, 1.00f, 1.00f, 1.0f), // text - pure white for contrast
        ImVec4(0.90f, 0.90f, 0.90f, 1.0f), // subtext1
        ImVec4(0.75f, 0.75f, 0.75f, 1.0f), // subtext0
        ImVec4(0.45f, 0.45f, 0.45f, 1.0f), // overlay2
        ImVec4(0.35f, 0.35f, 0.35f, 1.0f), // overlay1
        ImVec4(0.25f, 0.25f, 0.25f, 1.0f), // overlay0
        ImVec4(0.20f, 0.20f, 0.20f, 1.0f), // surface2
        ImVec4(0.15f, 0.15f, 0.15f, 1.0f), // surface1
        ImVec4(0.12f, 0.12f, 0.12f, 1.0f), // surface0
        ImVec4(0.08f, 0.08f, 0.08f, 1.0f), // base - darker for better contrast
        ImVec4(0.06f, 0.06f, 0.06f, 1.0f), // mantle
        ImVec4(0.04f, 0.04f, 0.04f, 1.0f)  // crust
    };

    // Native Light Theme (inspired by macOS/Windows light modes)
    const Colors NATIVE_LIGHT = {
        ImVec4(0.86f, 0.56f, 0.54f, 1.0f), // rosewater
        ImVec4(0.84f, 0.48f, 0.48f, 1.0f), // flamingo
        ImVec4(0.53f, 0.46f, 0.85f, 1.0f), // pink
        ImVec4(0.53f, 0.46f, 0.85f, 1.0f), // mauve
        ImVec4(0.82f, 0.26f, 0.26f, 1.0f), // red
        ImVec4(0.83f, 0.33f, 0.38f, 1.0f), // maroon
        ImVec4(0.98f, 0.55f, 0.33f, 1.0f), // peach
        ImVec4(0.85f, 0.66f, 0.13f, 1.0f), // yellow
        ImVec4(0.20f, 0.78f, 0.35f, 1.0f), // green - system green
        ImVec4(0.23f, 0.64f, 0.59f, 1.0f), // teal
        ImVec4(0.00f, 0.48f, 1.00f, 1.0f), // sky - system blue
        ImVec4(0.15f, 0.58f, 0.76f, 1.0f), // sapphire
        ImVec4(0.00f, 0.48f, 1.00f, 1.0f), // blue - system accent
        ImVec4(0.47f, 0.48f, 0.73f, 1.0f), // lavender
        ImVec4(0.00f, 0.00f, 0.00f, 1.0f), // text - pure black for contrast
        ImVec4(0.30f, 0.30f, 0.30f, 1.0f), // subtext1
        ImVec4(0.45f, 0.45f, 0.45f, 1.0f), // subtext0
        ImVec4(0.60f, 0.60f, 0.60f, 1.0f), // overlay2
        ImVec4(0.70f, 0.70f, 0.70f, 1.0f), // overlay1
        ImVec4(0.80f, 0.80f, 0.80f, 1.0f), // overlay0
        ImVec4(0.95f, 0.95f, 0.95f, 1.0f), // surface2
        ImVec4(0.97f, 0.97f, 0.97f, 1.0f), // surface1
        ImVec4(0.99f, 0.99f, 0.99f, 1.0f), // surface0
        ImVec4(1.00f, 1.00f, 1.00f, 1.0f), // base - pure white
        ImVec4(0.98f, 0.98f, 0.98f, 1.0f), // mantle
        ImVec4(0.96f, 0.96f, 0.96f, 1.0f)  // crust
    };

    // Legacy themes for compatibility
    const Colors MOCHA = NATIVE_DARK;
    const Colors LATTE = NATIVE_LIGHT;

    inline void ApplyNativeTheme(const Colors &colors, bool isLight = false) {
        ImGuiStyle &style = ImGui::GetStyle();

        // Native-like padding and spacing
        style.WindowPadding = ImVec2(16.0f, 16.0f);   // Larger padding for native feel
        style.FramePadding = ImVec2(8.0f, 6.0f);      // Taller frames like native controls
        style.CellPadding = ImVec2(8.0f, 6.0f);       // Consistent with frame padding
        style.ItemSpacing = ImVec2(8.0f, 8.0f);       // Tighter spacing like native apps
        style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);  // Consistent inner spacing
        style.TouchExtraPadding = ImVec2(0.0f, 0.0f); // No extra padding for desktop
        style.IndentSpacing = 20.0f;                  // Standard indent
        style.ScrollbarSize = 16.0f;                  // Standard scrollbar width
        style.GrabMinSize = 12.0f;                    // Minimum grab size

        // Platform-appropriate borders
        style.WindowBorderSize = 0.0f; // No window borders for modern look
        style.ChildBorderSize = 0.0f;  // No child borders
        style.PopupBorderSize = 1.0f;  // Subtle popup borders
        style.FrameBorderSize = 0.0f;  // No frame borders for clean look
        style.TabBorderSize = 0.0f;    // No tab borders

        // Platform-appropriate rounding
#ifdef __APPLE__
        // macOS likes more rounded corners
        style.WindowRounding = 10.0f;
        style.ChildRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 8.0f;
        style.ScrollbarRounding = 8.0f;
        style.GrabRounding = 6.0f;
        style.TabRounding = 6.0f;
#else
        // Windows/Linux prefers less rounding
        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;
#endif

        // Native-like alignment
        style.WindowTitleAlign = ImVec2(0.5f, 0.5f);    // Centered title
        style.WindowMenuButtonPosition = ImGuiDir_Left; // Standard position
        style.ColorButtonPosition = ImGuiDir_Right;     // Standard position
        style.ButtonTextAlign = ImVec2(0.5f, 0.5f);     // Centered button text
        style.SelectableTextAlign = ImVec2(0.0f, 0.5f); // Left-aligned, vertically centered

        // Enhanced styling for native look
        style.Alpha = 1.0f;                           // Full opacity
        style.DisabledAlpha = 0.5f;                   // Standard disabled alpha
        style.WindowMinSize = ImVec2(200.0f, 200.0f); // Reasonable minimum size

        // Colors with native appearance
        ImVec4 *colors_array = style.Colors;
        colors_array[ImGuiCol_Text] = colors.text;
        colors_array[ImGuiCol_TextDisabled] = colors.subtext0;
        colors_array[ImGuiCol_WindowBg] = colors.base;
        colors_array[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // Transparent
        colors_array[ImGuiCol_PopupBg] = colors.surface0;
        colors_array[ImGuiCol_Border] = colors.overlay0;
        colors_array[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // No shadow
        colors_array[ImGuiCol_FrameBg] = colors.surface0;
        colors_array[ImGuiCol_FrameBgHovered] = colors.surface1;
        colors_array[ImGuiCol_FrameBgActive] = colors.surface2;
        colors_array[ImGuiCol_TitleBg] = colors.mantle;
        colors_array[ImGuiCol_TitleBgActive] = colors.surface0;
        colors_array[ImGuiCol_TitleBgCollapsed] = colors.surface0;
        colors_array[ImGuiCol_MenuBarBg] = colors.base;
        colors_array[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // Transparent
        colors_array[ImGuiCol_ScrollbarGrab] = colors.overlay0;
        colors_array[ImGuiCol_ScrollbarGrabHovered] = colors.overlay1;
        colors_array[ImGuiCol_ScrollbarGrabActive] = colors.overlay2;
        colors_array[ImGuiCol_CheckMark] = colors.blue;
        colors_array[ImGuiCol_SliderGrab] = colors.blue;
        colors_array[ImGuiCol_SliderGrabActive] = colors.sky;
        colors_array[ImGuiCol_Button] = colors.surface0;
        colors_array[ImGuiCol_ButtonHovered] = colors.surface1;
        colors_array[ImGuiCol_ButtonActive] = colors.surface2;
        colors_array[ImGuiCol_Header] = colors.surface0;
        colors_array[ImGuiCol_HeaderHovered] = colors.surface1;
        colors_array[ImGuiCol_HeaderActive] = colors.surface2;
        colors_array[ImGuiCol_Separator] = colors.overlay0;
        colors_array[ImGuiCol_SeparatorHovered] = colors.overlay1;
        colors_array[ImGuiCol_SeparatorActive] = colors.blue;
        colors_array[ImGuiCol_ResizeGrip] = colors.overlay0;
        colors_array[ImGuiCol_ResizeGripHovered] = colors.overlay1;
        colors_array[ImGuiCol_ResizeGripActive] = colors.blue;
        colors_array[ImGuiCol_Tab] = colors.mantle;
        colors_array[ImGuiCol_TabHovered] = colors.surface1;
        colors_array[ImGuiCol_TabActive] = colors.surface0;
        colors_array[ImGuiCol_TabUnfocused] = colors.mantle;
        colors_array[ImGuiCol_TabUnfocusedActive] = colors.surface0;
        colors_array[ImGuiCol_DockingPreview] =
            ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.3f);
        colors_array[ImGuiCol_DockingEmptyBg] = colors.base;
        colors_array[ImGuiCol_PlotLines] = colors.text;
        colors_array[ImGuiCol_PlotLinesHovered] = colors.blue;
        colors_array[ImGuiCol_PlotHistogram] = colors.blue;
        colors_array[ImGuiCol_PlotHistogramHovered] = colors.sky;
        colors_array[ImGuiCol_TableHeaderBg] = colors.surface0;
        colors_array[ImGuiCol_TableBorderStrong] = colors.overlay1;
        colors_array[ImGuiCol_TableBorderLight] = colors.overlay0;
        colors_array[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // Transparent
        colors_array[ImGuiCol_TableRowBgAlt] =
            ImVec4(colors.surface0.x, colors.surface0.y, colors.surface0.z, 0.5f);
        colors_array[ImGuiCol_TextSelectedBg] =
            ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.3f);
        colors_array[ImGuiCol_DragDropTarget] =
            ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.8f);
        colors_array[ImGuiCol_NavHighlight] = colors.blue;
        colors_array[ImGuiCol_NavWindowingHighlight] = colors.blue;
        colors_array[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
        colors_array[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.4f);
    }

    // Legacy function for backward compatibility
    inline void ApplyTheme(const Colors &colors) {
        ApplyNativeTheme(colors, false);
    }
} // namespace Theme
