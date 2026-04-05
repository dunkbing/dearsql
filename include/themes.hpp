#pragma once
#include "imgui.h"

namespace Theme {
    struct Colors {
        ImVec4 rose;
        ImVec4 coral;
        ImVec4 magenta;
        ImVec4 purple;
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

    // Zed One Dark
    constexpr Colors ONE_DARK = {
        ImVec4(0.88f, 0.67f, 0.69f, 1.0f), // rose       #e0abb0
        ImVec4(0.84f, 0.51f, 0.56f, 1.0f), // coral      #d5818e
        ImVec4(0.84f, 0.37f, 0.87f, 1.0f), // magenta    #d55fde
        ImVec4(0.78f, 0.47f, 0.87f, 1.0f), // purple     #c678dd
        ImVec4(0.88f, 0.42f, 0.46f, 1.0f), // red        #e06c75
        ImVec4(0.75f, 0.31f, 0.27f, 1.0f), // maroon     #be5046
        ImVec4(0.82f, 0.60f, 0.40f, 1.0f), // peach      #d19a66
        ImVec4(0.90f, 0.75f, 0.48f, 1.0f), // yellow     #e5c07b
        ImVec4(0.60f, 0.76f, 0.47f, 1.0f), // green      #98c379
        ImVec4(0.34f, 0.71f, 0.76f, 1.0f), // teal       #56b6c2
        ImVec4(0.38f, 0.69f, 0.94f, 1.0f), // sky        #61afef
        ImVec4(0.17f, 0.73f, 0.77f, 1.0f), // sapphire   #2bbac5
        ImVec4(0.38f, 0.69f, 0.94f, 1.0f), // blue       #61afef
        ImVec4(0.32f, 0.55f, 1.00f, 1.0f), // lavender   #528bff
        ImVec4(0.67f, 0.70f, 0.75f, 1.0f), // text       #abb2bf
        ImVec4(0.62f, 0.65f, 0.71f, 1.0f), // subtext1   #9da5b4
        ImVec4(0.51f, 0.54f, 0.59f, 1.0f), // subtext0   #828997
        ImVec4(0.39f, 0.43f, 0.51f, 1.0f), // overlay2   #636d83
        ImVec4(0.36f, 0.39f, 0.44f, 1.0f), // overlay1   #5c6370
        ImVec4(0.29f, 0.32f, 0.39f, 1.0f), // overlay0   #4b5263
        ImVec4(0.24f, 0.26f, 0.31f, 1.0f), // surface2   #3d4350
        ImVec4(0.21f, 0.23f, 0.27f, 1.0f), // surface1   #353b45
        ImVec4(0.17f, 0.19f, 0.24f, 1.0f), // surface0   #2c313c
        ImVec4(0.16f, 0.17f, 0.20f, 1.0f), // base       #282c34
        ImVec4(0.13f, 0.15f, 0.17f, 1.0f), // mantle     #21252b
        ImVec4(0.10f, 0.11f, 0.14f, 1.0f)  // crust      #1a1d23
    };

    // Zed One Light
    constexpr Colors ONE_LIGHT = {
        ImVec4(0.88f, 0.38f, 0.44f, 1.0f), // rose       #e06070
        ImVec4(0.82f, 0.25f, 0.38f, 1.0f), // coral      #d04060
        ImVec4(0.77f, 0.27f, 0.61f, 1.0f), // magenta    #c4449c
        ImVec4(0.65f, 0.15f, 0.64f, 1.0f), // purple     #a626a4
        ImVec4(0.89f, 0.34f, 0.29f, 1.0f), // red        #e45649
        ImVec4(0.79f, 0.07f, 0.26f, 1.0f), // maroon     #ca1243
        ImVec4(0.84f, 0.37f, 0.00f, 1.0f), // peach      #d75f00
        ImVec4(0.76f, 0.51f, 0.00f, 1.0f), // yellow     #c18401
        ImVec4(0.31f, 0.63f, 0.31f, 1.0f), // green      #50a14f
        ImVec4(0.00f, 0.52f, 0.74f, 1.0f), // teal       #0184bc
        ImVec4(0.25f, 0.47f, 0.95f, 1.0f), // sky        #4078f2
        ImVec4(0.13f, 0.63f, 0.77f, 1.0f), // sapphire   #20a0c5
        ImVec4(0.25f, 0.47f, 0.95f, 1.0f), // blue       #4078f2
        ImVec4(0.45f, 0.36f, 0.87f, 1.0f), // lavender   #735cde
        ImVec4(0.22f, 0.23f, 0.26f, 1.0f), // text       #383a42
        ImVec4(0.29f, 0.33f, 0.39f, 1.0f), // subtext1   #4b5563
        ImVec4(0.41f, 0.42f, 0.47f, 1.0f), // subtext0   #696c77
        ImVec4(0.63f, 0.63f, 0.65f, 1.0f), // overlay2   #a0a1a7
        ImVec4(0.72f, 0.72f, 0.72f, 1.0f), // overlay1   #b8b8b8
        ImVec4(0.83f, 0.83f, 0.83f, 1.0f), // overlay0   #d4d4d4
        ImVec4(0.82f, 0.82f, 0.82f, 1.0f), // surface2   #d0d0d0 (borders/dividers)
        ImVec4(0.88f, 0.88f, 0.88f, 1.0f), // surface1   #e0e0e0
        ImVec4(0.93f, 0.93f, 0.94f, 1.0f), // surface0   #edeef0
        ImVec4(0.98f, 0.98f, 0.98f, 1.0f), // base       #fafafa
        ImVec4(0.94f, 0.94f, 0.94f, 1.0f), // mantle     #f0f0f0
        ImVec4(0.90f, 0.90f, 0.91f, 1.0f)  // crust      #e6e6e8
    };

    constexpr Colors NATIVE_DARK = ONE_DARK;
    constexpr Colors NATIVE_LIGHT = ONE_LIGHT;
    constexpr Colors MOCHA = ONE_DARK;
    constexpr Colors LATTE = ONE_LIGHT;

    inline void ApplyNativeTheme(const Colors& colors) {
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.FramePadding = ImVec2(8.0f, 6.0f);
        style.CellPadding = ImVec2(8.0f, 6.0f);
        style.ItemSpacing = ImVec2(8.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
        style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
        style.IndentSpacing = 20.0f;
        style.ScrollbarSize = 16.0f;
        style.GrabMinSize = 12.0f;

        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 1.0f;

        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 0.0f;
        style.PopupRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding = 0.0f;
        style.TabRounding = 0.0f;

        style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_Left;
        style.ColorButtonPosition = ImGuiDir_Right;
        style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
        style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

        style.Alpha = 1.0f;
        style.DisabledAlpha = 0.5f;
        style.WindowMinSize = ImVec2(200.0f, 200.0f);

        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = colors.text;
        c[ImGuiCol_InputTextCursor] = colors.text;
        c[ImGuiCol_TextDisabled] = colors.subtext0;
        c[ImGuiCol_TextLink] = colors.blue;
        c[ImGuiCol_WindowBg] = colors.base;
        c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_PopupBg] = colors.surface0;
        c[ImGuiCol_Border] = colors.overlay0;
        c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_FrameBg] = colors.surface0;
        c[ImGuiCol_FrameBgHovered] = colors.surface1;
        c[ImGuiCol_FrameBgActive] = colors.surface2;
        c[ImGuiCol_TitleBg] = colors.mantle;
        c[ImGuiCol_TitleBgActive] = colors.surface0;
        c[ImGuiCol_TitleBgCollapsed] = colors.surface0;
        c[ImGuiCol_MenuBarBg] = colors.base;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_ScrollbarGrab] = colors.overlay0;
        c[ImGuiCol_ScrollbarGrabHovered] = colors.overlay1;
        c[ImGuiCol_ScrollbarGrabActive] = colors.overlay2;
        c[ImGuiCol_CheckMark] = colors.blue;
        c[ImGuiCol_SliderGrab] = colors.blue;
        c[ImGuiCol_SliderGrabActive] = colors.sky;
        c[ImGuiCol_Button] = colors.surface0;
        c[ImGuiCol_ButtonHovered] = colors.surface1;
        c[ImGuiCol_ButtonActive] = colors.surface2;
        c[ImGuiCol_Header] = colors.surface0;
        c[ImGuiCol_HeaderHovered] = colors.surface1;
        c[ImGuiCol_HeaderActive] = colors.surface2;
        c[ImGuiCol_Separator] = colors.overlay0;
        c[ImGuiCol_SeparatorHovered] = colors.overlay1;
        c[ImGuiCol_SeparatorActive] = colors.blue;
        c[ImGuiCol_ResizeGrip] = colors.overlay0;
        c[ImGuiCol_ResizeGripHovered] = colors.overlay1;
        c[ImGuiCol_ResizeGripActive] = colors.blue;
        c[ImGuiCol_Tab] = colors.mantle;
        c[ImGuiCol_TabHovered] = colors.surface1;
        c[ImGuiCol_TabSelected] = colors.surface0;
        c[ImGuiCol_TabSelectedOverline] = colors.blue;
        c[ImGuiCol_TabDimmed] = colors.mantle;
        c[ImGuiCol_TabDimmedSelected] = colors.surface0;
        c[ImGuiCol_TabDimmedSelectedOverline] = colors.overlay0;
        c[ImGuiCol_DockingPreview] = ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.3f);
        c[ImGuiCol_DockingEmptyBg] = colors.base;
        c[ImGuiCol_PlotLines] = colors.text;
        c[ImGuiCol_PlotLinesHovered] = colors.blue;
        c[ImGuiCol_PlotHistogram] = colors.blue;
        c[ImGuiCol_PlotHistogramHovered] = colors.sky;
        c[ImGuiCol_TableHeaderBg] = colors.surface0;
        c[ImGuiCol_TableBorderStrong] = colors.overlay1;
        c[ImGuiCol_TableBorderLight] = colors.overlay0;
        c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_TableRowBgAlt] =
            ImVec4(colors.surface0.x, colors.surface0.y, colors.surface0.z, 0.5f);
        c[ImGuiCol_TreeLines] = colors.overlay0;
        c[ImGuiCol_TextSelectedBg] = ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.3f);
        c[ImGuiCol_DragDropTarget] = ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.8f);
        c[ImGuiCol_DragDropTargetBg] =
            ImVec4(colors.green.x, colors.green.y, colors.green.z, 0.15f);
        c[ImGuiCol_UnsavedMarker] = colors.yellow;
        c[ImGuiCol_NavCursor] = colors.blue;
        c[ImGuiCol_NavWindowingHighlight] = colors.blue;
        c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.4f);
    }

    inline void ApplyTheme(const Colors& colors) {
        ApplyNativeTheme(colors);
    }

    namespace Spacing {
        constexpr float XS = 2.0f;
        constexpr float S = 4.0f;
        constexpr float M = 8.0f;
        constexpr float L = 16.0f;
        constexpr float XL = 20.0f;
    } // namespace Spacing
} // namespace Theme
