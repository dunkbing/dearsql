#pragma once

#include <functional>

class Application;

// Abstract titlebar/toolbar interface — one platform implementation each
class ITitlebar {
public:
    virtual ~ITitlebar() = default;

    // Create widgets / controls and attach to the window
    virtual void setup() = 0;

    // Draw the titlebar using ImGui (Windows only; no-op on Linux/macOS)
    virtual void render() = 0;

    // Draw titlebar popups using ImGui (Windows only; no-op on Linux/macOS)
    virtual void renderPopups() = 0;

    // Synchronise the workspace dropdown / list with the current app state
    virtual void updateWorkspaceDropdown() = 0;

    // Apply light/dark theme to native chrome (GTK CSS, NSAppearance, DWM color…)
    virtual void applyTheme(bool isDark) = 0;

    // Logical height of the custom titlebar in points/DIPs (0 for native bars)
    virtual float getHeight() const = 0;

    // Top inset that the client area must respect (non-zero only on Windows)
    virtual float getClientAreaTopInset() const {
        return 0.0f;
    }
};

// ---- Linux: imgui bar, window buttons from gtk-decoration-layout (mydear) ----
#if defined(__linux__)

#include <gtk/gtk.h>
#include <vector>

class LinuxTitlebar final : public ITitlebar {
public:
    LinuxTitlebar(Application* app, GtkWidget* parentWindow);

    void setup() override;
    // the shared imgui bar (ui/title_bar.hpp) in mydear's title bar strip
    void render() override;
    void renderPopups() override {}
    void updateWorkspaceDropdown() override {} // the bar reads app state each frame
    // gtk css for the native dialogs (mydear sets prefer-dark)
    void applyTheme(bool isDark) override;
    float getHeight() const override;
    // the content runs under the bar
    float getClientAreaTopInset() const override {
        return getHeight();
    }

    void showLicenseDialog();

private:
    Application* app_ = nullptr;
    GtkWidget* parentWindow_ = nullptr;
};

// ---- macOS: imgui bar, native traffic lights (mydear) ----
#elif defined(__APPLE__)

#include <GLFW/glfw3.h>

#ifdef __OBJC__
@class MacOSTitlebarDelegate;
#else
typedef void MacOSTitlebarDelegate;
#endif

class MacOSTitlebar final : public ITitlebar {
public:
    MacOSTitlebar(Application* app, GLFWwindow* window);
    ~MacOSTitlebar() override;

    void setup() override;
    // the shared imgui bar (ui/title_bar.hpp) in mydear's title bar strip
    void render() override;
    void renderPopups() override {}
    void updateWorkspaceDropdown() override {} // the bar reads app state each frame
    void applyTheme(bool isDark) override;
    float getHeight() const override;
    // the content runs under the bar
    float getClientAreaTopInset() const override {
        return getHeight();
    }

private:
    Application* app_ = nullptr;
    GLFWwindow* window_ = nullptr;
    MacOSTitlebarDelegate* delegate_ = nullptr;
};

// ---- Windows: custom ImGui-drawn frame ----
#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "imgui.h"
#include <windows.h>

struct GLFWwindow;

class WindowsTitlebar final : public ITitlebar {
public:
    WindowsTitlebar(Application* app, GLFWwindow* window);

    void setup() override;
    // the shared imgui bar (ui/title_bar.hpp) in mydear's title bar strip
    void render() override;
    void renderPopups() override {}
    void updateWorkspaceDropdown() override {} // the bar reads app state each frame
    void applyTheme(bool) override {}          // mydear follows the theme (dwm dark mode)
    float getHeight() const override;
    // the content runs under the bar
    float getClientAreaTopInset() const override {
        return getHeight();
    }

    void showLicenseDialog();

private:
    Application* app_ = nullptr;
    GLFWwindow* window_ = nullptr;
};

#endif
