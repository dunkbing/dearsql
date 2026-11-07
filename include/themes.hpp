#pragma once
#include "imgui.h"
#include <string>
#include <string_view>

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

    // #rrggbb as a color
    constexpr ImVec4 hex(unsigned rgb) {
        return ImVec4(static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                      static_cast<float>(rgb & 0xff) / 255.0f, 1.0f);
    }

    // each theme's own palette, mapped onto the slots above: accents first, then text,
    // overlay (muted text, borders), surface (popups, hover), base (tabs), mantle
    // (sidebar, title bar) and crust (darkest edge). light themes run the other way
    // clang-format off
    constexpr Colors AYU_DARK = {
        hex(0xf29668), hex(0xf07178), hex(0xd2a6ff), hex(0xd2a6ff), hex(0xf07178), hex(0xd95757),
        hex(0xff8f40), hex(0xffb454), hex(0xaad94c), hex(0x95e6cb), hex(0x39bae6), hex(0x39bae6),
        hex(0x59c2ff), hex(0x73b8ff), hex(0xbfbdb6), hex(0xacb6bf), hex(0x8a9199), hex(0x6c7380),
        hex(0x565b66), hex(0x3d424d), hex(0x2a2f3a), hex(0x1f2430), hex(0x171b24), hex(0x0d1017),
        hex(0x0b0e14), hex(0x080a0f)};
    constexpr Colors AYU_MIRAGE = {
        hex(0xf29e74), hex(0xf28779), hex(0xdfbfff), hex(0xdfbfff), hex(0xf28779), hex(0xff6666),
        hex(0xffad66), hex(0xffd173), hex(0xd5ff80), hex(0x95e6cb), hex(0x5ccfe6), hex(0x5ccfe6),
        hex(0x73d0ff), hex(0x80bfff), hex(0xcccac2), hex(0xb8b6ad), hex(0x8a9199), hex(0x707a8c),
        hex(0x5c6773), hex(0x4b5466), hex(0x3d4455), hex(0x33394a), hex(0x2a3040), hex(0x242936),
        hex(0x1f2430), hex(0x1a1f29)};
    constexpr Colors AYU_LIGHT = {
        hex(0xed9366), hex(0xf07171), hex(0xa37acc), hex(0xa37acc), hex(0xe65050), hex(0xd14b4b),
        hex(0xfa8d3e), hex(0xf2ae49), hex(0x86b300), hex(0x4cbf99), hex(0x55b4d4), hex(0x55b4d4),
        hex(0x399ee6), hex(0x478acc), hex(0x5c6166), hex(0x6b7075), hex(0x8a9199), hex(0xa0a6ad),
        hex(0xb8bec4), hex(0xcfd3d8), hex(0xd8dce0), hex(0xe3e6ea), hex(0xeceff2), hex(0xfcfcfc),
        hex(0xf3f4f5), hex(0xe7eaed)};
    constexpr Colors GRUVBOX_DARK = {
        hex(0xd3869b), hex(0xea6962), hex(0xd3869b), hex(0xb16286), hex(0xfb4934), hex(0xcc241d),
        hex(0xfe8019), hex(0xfabd2f), hex(0xb8bb26), hex(0x8ec07c), hex(0x83a598), hex(0x689d6a),
        hex(0x83a598), hex(0x7daea3), hex(0xebdbb2), hex(0xd5c4a1), hex(0xbdae93), hex(0xa89984),
        hex(0x928374), hex(0x7c6f64), hex(0x665c54), hex(0x504945), hex(0x3c3836), hex(0x282828),
        hex(0x1d2021), hex(0x141617)};
    constexpr Colors GRUVBOX_LIGHT = {
        hex(0xb16286), hex(0xcc241d), hex(0xb16286), hex(0x8f3f71), hex(0xcc241d), hex(0x9d0006),
        hex(0xd65d0e), hex(0xb57614), hex(0x79740e), hex(0x427b58), hex(0x458588), hex(0x689d6a),
        hex(0x076678), hex(0x458588), hex(0x3c3836), hex(0x504945), hex(0x665c54), hex(0x7c6f64),
        hex(0x928374), hex(0xa89984), hex(0xbdae93), hex(0xd5c4a1), hex(0xebdbb2), hex(0xfbf1c7),
        hex(0xf2e5bc), hex(0xebdbb2)};
    constexpr Colors VSCODE_DARK = {
        hex(0xce9178), hex(0xf48771), hex(0xc586c0), hex(0xc586c0), hex(0xf14c4c), hex(0xd16969),
        hex(0xce9178), hex(0xdcdcaa), hex(0x89d185), hex(0x4ec9b0), hex(0x9cdcfe), hex(0x4fc1ff),
        hex(0x569cd6), hex(0x0078d4), hex(0xcccccc), hex(0xbbbbbb), hex(0x9d9d9d), hex(0x858585),
        hex(0x6e7681), hex(0x454545), hex(0x3c3c3c), hex(0x313131), hex(0x2b2b2b), hex(0x1f1f1f),
        hex(0x181818), hex(0x111111)};
    constexpr Colors VSCODE_LIGHT = {
        hex(0xb05a78), hex(0xcd3131), hex(0xaf00db), hex(0xaf00db), hex(0xe51400), hex(0xa31515),
        hex(0xb8510f), hex(0xbf8803), hex(0x388a34), hex(0x267f99), hex(0x0070c1), hex(0x0451a5),
        hex(0x005fb8), hex(0x4b69c6), hex(0x3b3b3b), hex(0x4f4f4f), hex(0x616161), hex(0x8a8a8a),
        hex(0xa5a5a5), hex(0xc4c4c4), hex(0xcecece), hex(0xe5e5e5), hex(0xeeeeee), hex(0xffffff),
        hex(0xf8f8f8), hex(0xefefef)};
    constexpr Colors CATPPUCCIN_MOCHA = {
        hex(0xf5e0dc), hex(0xf2cdcd), hex(0xf5c2e7), hex(0xcba6f7), hex(0xf38ba8), hex(0xeba0ac),
        hex(0xfab387), hex(0xf9e2af), hex(0xa6e3a1), hex(0x94e2d5), hex(0x89dceb), hex(0x74c7ec),
        hex(0x89b4fa), hex(0xb4befe), hex(0xcdd6f4), hex(0xbac2de), hex(0xa6adc8), hex(0x9399b2),
        hex(0x7f849c), hex(0x6c7086), hex(0x585b70), hex(0x45475a), hex(0x313244), hex(0x1e1e2e),
        hex(0x181825), hex(0x11111b)};
    constexpr Colors CATPPUCCIN_LATTE = {
        hex(0xdc8a78), hex(0xdd7878), hex(0xea76cb), hex(0x8839ef), hex(0xd20f39), hex(0xe64553),
        hex(0xfe640b), hex(0xdf8e1d), hex(0x40a02b), hex(0x179299), hex(0x04a5e5), hex(0x209fb5),
        hex(0x1e66f5), hex(0x7287fd), hex(0x4c4f69), hex(0x5c5f77), hex(0x6c6f85), hex(0x7c7f93),
        hex(0x8c8fa1), hex(0x9ca0b0), hex(0xacb0be), hex(0xbcc0cc), hex(0xccd0da), hex(0xeff1f5),
        hex(0xe6e9ef), hex(0xdce0e8)};
    constexpr Colors TOKYO_NIGHT = {
        hex(0xf7768e), hex(0xff757f), hex(0xbb9af7), hex(0x9d7cd8), hex(0xf7768e), hex(0xdb4b4b),
        hex(0xff9e64), hex(0xe0af68), hex(0x9ece6a), hex(0x73daca), hex(0x7dcfff), hex(0x2ac3de),
        hex(0x7aa2f7), hex(0xb4bdf8), hex(0xc0caf5), hex(0xa9b1d6), hex(0x9aa5ce), hex(0x737aa2),
        hex(0x565f89), hex(0x545c7e), hex(0x414868), hex(0x3b4261), hex(0x292e42), hex(0x1a1b26),
        hex(0x16161e), hex(0x101014)};
    constexpr Colors NORD = {
        hex(0xd08770), hex(0xbf616a), hex(0xb48ead), hex(0xb48ead), hex(0xbf616a), hex(0xa5545c),
        hex(0xd08770), hex(0xebcb8b), hex(0xa3be8c), hex(0x8fbcbb), hex(0x88c0d0), hex(0x88c0d0),
        hex(0x81a1c1), hex(0x5e81ac), hex(0xd8dee9), hex(0xc8ced9), hex(0xa7afbe), hex(0x8a93a6),
        hex(0x6d778b), hex(0x616e88), hex(0x4c566a), hex(0x434c5e), hex(0x3b4252), hex(0x2e3440),
        hex(0x2a2f3a), hex(0x242933)};
    constexpr Colors DRACULA = {
        hex(0xff92df), hex(0xff6e6e), hex(0xff79c6), hex(0x9580ff), hex(0xff5555), hex(0xff6e6e),
        hex(0xffb86c), hex(0xf1fa8c), hex(0x50fa7b), hex(0x8be9fd), hex(0x8be9fd), hex(0x62d6e8),
        hex(0xbd93f9), hex(0xbd93f9), hex(0xf8f8f2), hex(0xe2e2dc), hex(0xbfbfbf), hex(0x9ea8c7),
        hex(0x6272a4), hex(0x545772), hex(0x4d5066), hex(0x44475a), hex(0x343746), hex(0x282a36),
        hex(0x21222c), hex(0x191a21)};
    constexpr Colors SOLARIZED_DARK = {
        hex(0xd33682), hex(0xdc322f), hex(0xd33682), hex(0x6c71c4), hex(0xdc322f), hex(0xa12a26),
        hex(0xcb4b16), hex(0xb58900), hex(0x859900), hex(0x2aa198), hex(0x2aa198), hex(0x268bd2),
        hex(0x268bd2), hex(0x6c71c4), hex(0x93a1a1), hex(0x839496), hex(0x657b83), hex(0x586e75),
        hex(0x4a5f66), hex(0x37515a), hex(0x1e4652), hex(0x103e4a), hex(0x073642), hex(0x002b36),
        hex(0x00252e), hex(0x001e26)};
    constexpr Colors SOLARIZED_LIGHT = {
        hex(0xd33682), hex(0xdc322f), hex(0xd33682), hex(0x6c71c4), hex(0xdc322f), hex(0xa12a26),
        hex(0xcb4b16), hex(0xb58900), hex(0x859900), hex(0x2aa198), hex(0x2aa198), hex(0x268bd2),
        hex(0x268bd2), hex(0x6c71c4), hex(0x586e75), hex(0x657b83), hex(0x839496), hex(0x93a1a1),
        hex(0xb3b8ad), hex(0xcfcfc0), hex(0xdcd8c4), hex(0xe6e0cc), hex(0xeee8d5), hex(0xfdf6e3),
        hex(0xf5efdc), hex(0xeee8d5)};
    constexpr Colors GITHUB_DARK = {
        hex(0xffa198), hex(0xff7b72), hex(0xdb61a2), hex(0xd2a8ff), hex(0xf85149), hex(0xda3633),
        hex(0xffa657), hex(0xd29922), hex(0x3fb950), hex(0x56d4dd), hex(0xa5d6ff), hex(0x79c0ff),
        hex(0x58a6ff), hex(0x2f81f7), hex(0xe6edf3), hex(0xc9d1d9), hex(0x8b949e), hex(0x7d8590),
        hex(0x6e7681), hex(0x484f58), hex(0x30363d), hex(0x21262d), hex(0x161b22), hex(0x0d1117),
        hex(0x010409), hex(0x000000)};
    constexpr Colors GITHUB_LIGHT = {
        hex(0xbf3989), hex(0xcf222e), hex(0xbf3989), hex(0x8250df), hex(0xd1242f), hex(0xa40e26),
        hex(0xbc4c00), hex(0x9a6700), hex(0x1a7f37), hex(0x1b7c83), hex(0x0550ae), hex(0x0a3069),
        hex(0x0969da), hex(0x6639ba), hex(0x1f2328), hex(0x424a53), hex(0x656d76), hex(0x818b98),
        hex(0xafb8c1), hex(0xd0d7de), hex(0xd8dee4), hex(0xeaeef2), hex(0xf6f8fa), hex(0xffffff),
        hex(0xf6f8fa), hex(0xeaeef2)};
    // clang-format on

    struct ThemeDef {
        const char* id; // stored in settings
        const char* name;
        bool dark;
        Colors colors;
    };

    inline constexpr ThemeDef THEMES[] = {
        {"one-dark", "One Dark", true, ONE_DARK},
        {"one-light", "One Light", false, ONE_LIGHT},
        {"ayu-dark", "Ayu Dark", true, AYU_DARK},
        {"ayu-mirage", "Ayu Mirage", true, AYU_MIRAGE},
        {"ayu-light", "Ayu Light", false, AYU_LIGHT},
        {"gruvbox-dark", "Gruvbox Dark", true, GRUVBOX_DARK},
        {"gruvbox-light", "Gruvbox Light", false, GRUVBOX_LIGHT},
        {"vscode-dark", "VS Code Dark", true, VSCODE_DARK},
        {"vscode-light", "VS Code Light", false, VSCODE_LIGHT},
        {"catppuccin-mocha", "Catppuccin Mocha", true, CATPPUCCIN_MOCHA},
        {"catppuccin-latte", "Catppuccin Latte", false, CATPPUCCIN_LATTE},
        {"tokyo-night", "Tokyo Night", true, TOKYO_NIGHT},
        {"nord", "Nord", true, NORD},
        {"dracula", "Dracula", true, DRACULA},
        {"solarized-dark", "Solarized Dark", true, SOLARIZED_DARK},
        {"solarized-light", "Solarized Light", false, SOLARIZED_LIGHT},
        {"github-dark", "GitHub Dark", true, GITHUB_DARK},
        {"github-light", "GitHub Light", false, GITHUB_LIGHT},
    };

    // by settings id; "dark"/"light" are what older versions stored
    inline const ThemeDef& themeById(std::string_view id) {
        if (id == "light")
            id = "one-light";
        for (const auto& t : THEMES)
            if (id == t.id)
                return t;
        return THEMES[0];
    }


    namespace CornerRadius {
        constexpr float XSMALL = 2.0f;
        constexpr float SMALL = 4.0f;
        constexpr float MEDIUM = 6.0f;
        constexpr float LARGE = 8.0f;
    } // namespace CornerRadius

    inline void ApplyNativeTheme(const Colors& colors) {
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.FramePadding = ImVec2(8.0f, 6.0f);
        style.CellPadding = ImVec2(8.0f, 6.0f);
        style.ItemSpacing = ImVec2(8.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
        style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
        style.IndentSpacing = 20.0f;
        style.ScrollbarSize = 14.0f;
        style.GrabMinSize = 12.0f;

        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
        style.TabBorderSize = 0.0f;
        style.TabBarOverlineSize = 0.0f;
        style.DockingSeparatorSize = 1.0f; // the sidebar handle: hover padding keeps it easy to grab

        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = CornerRadius::SMALL;
        style.PopupRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding = CornerRadius::SMALL;
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
        // tinted towards the text so buttons stand out on windows and popups in
        // both themes (the surface steps are too close, and not ordered in light)
        auto tint = [&](float t) {
            const ImVec4& a = colors.surface0;
            const ImVec4& b = colors.text;
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                          1.0f);
        };
        c[ImGuiCol_Button] = tint(0.14f);
        c[ImGuiCol_ButtonHovered] = tint(0.22f);
        c[ImGuiCol_ButtonActive] = tint(0.30f);
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

    // Connections store a palette key rather than a colour, so a choice made in
    // one theme stays legible in the other.
    namespace ConnectionPalette {
        struct Entry {
            const char* key;
            ImVec4 Colors::* member;
        };

        inline constexpr Entry ENTRIES[] = {
            {"red", &Colors::red},     {"peach", &Colors::peach}, {"yellow", &Colors::yellow},
            {"green", &Colors::green}, {"blue", &Colors::blue},   {"purple", &Colors::purple},
        };

        // Returns false when the key is empty or unknown, leaving `out` untouched.
        inline bool resolve(const std::string& key, const Colors& colors, ImVec4& out) {
            if (key.empty())
                return false;
            for (const auto& entry : ENTRIES) {
                if (key == entry.key) {
                    out = colors.*entry.member;
                    return true;
                }
            }
            return false;
        }
    } // namespace ConnectionPalette

    namespace Spacing {
        constexpr float XS = 2.0f;
        constexpr float S = 4.0f;
        constexpr float M = 8.0f;
        constexpr float L = 16.0f;
        constexpr float XL = 20.0f;
    } // namespace Spacing
} // namespace Theme
