#pragma once

class Application;

// the app's bar inside mydear's title bar strip: sidebar toggle, new connection, title,
// workspace picker, settings. every desktop
namespace TitleBarView {
    void render(Application& app);
}

// macos_bar_buttons.mm: an NSButton with an SF Symbol; a click sets *clicked
#include "imgui.h"
namespace TitleBarView {
    void* makeNativeButton(const char* symbol, const char* tooltip, bool* clicked);
    // tint (and title, when given); returns the width the button wants
    float updateNativeButton(void* button, const char* title, ImU32 tint);
} // namespace TitleBarView
