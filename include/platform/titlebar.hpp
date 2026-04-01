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

// ---- Linux: native GTK HeaderBar ----
#if defined(__linux__)

#include <gtk/gtk.h>
#include <vector>

class LinuxTitlebar final : public ITitlebar {
public:
    LinuxTitlebar(Application* app, GtkWidget* parentWindow);

    // Called by the platform so the titlebar can call noteInteraction()
    void setInteractionCallback(std::function<void()> cb) {
        interactionCb_ = std::move(cb);
    }

    void setup() override;
    // Native GTK — no ImGui rendering needed
    void render() override {}
    void renderPopups() override {}
    void updateWorkspaceDropdown() override;
    void applyTheme(bool isDark) override;
    float getHeight() const override {
        return 0.0f;
    }

    // Expose for the platform to show/hide the update badge
    GtkWidget* getUpdateButton() const {
        return updateButton_;
    }
    GtkWidget* getHeaderBar() const {
        return headerBar_;
    }

    void showLicenseDialog();
    void showCreateWorkspaceDialog();

    // GTK signal callbacks (static — userData is LinuxTitlebar*)
    static void onSidebarToggle(GtkButton*, gpointer);
    static void onAddConnection(GtkButton*, gpointer);
    static void onThemeLightClicked(GtkButton*, gpointer);
    static void onThemeDarkClicked(GtkButton*, gpointer);
    static void onThemeAutoClicked(GtkButton*, gpointer);
    static void onLicenseClicked(GtkButton*, gpointer);

private:
    void updateThemeButtons();

    Application* app_ = nullptr;
    GtkWidget* parentWindow_ = nullptr;

    GtkWidget* headerBar_ = nullptr;
    GtkWidget* sidebarButton_ = nullptr;
    GtkWidget* workspaceButton_ = nullptr;
    GtkWidget* workspacePopover_ = nullptr;
    GtkWidget* workspaceItemsBox_ = nullptr;
    GtkWidget* addButton_ = nullptr;
    GtkWidget* menuButton_ = nullptr;
    GtkWidget* menuPopover_ = nullptr;
    GtkWidget* updateButton_ = nullptr;
    GtkWidget* themeLightButton_ = nullptr;
    GtkWidget* themeDarkButton_ = nullptr;
    GtkWidget* themeAutoButton_ = nullptr;
    GtkWidget* licenseButton_ = nullptr;
    GtkWidget* fontSizeLabel_ = nullptr;

    std::vector<int> workspaceIdsByIndex_;
    std::function<void()> interactionCb_;
};

// ---- macOS: native NSToolbar ----
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
    void render() override {}
    void renderPopups() override {}
    void updateWorkspaceDropdown() override;
    void applyTheme(bool isDark) override;
    float getHeight() const override {
        return 28.0f;
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
    void render() override;
    void renderPopups() override;
    void updateWorkspaceDropdown() override {} // reads app state directly each frame
    void applyTheme(bool isDark) override;
    float getHeight() const override {
        return 32.0f;
    }
    float getClientAreaTopInset() const override {
        return getHeight();
    }

    // Called from WndProc for custom hit testing
    LRESULT hitTest(HWND hWnd, LPARAM lParam) const;

    // True when the theme changed this frame (platform should re-apply DWM colour)
    bool themeChanged() const {
        return themeChanged_;
    }
    void clearThemeChanged() {
        themeChanged_ = false;
    }

    void showLicenseDialog();
    void showCreateWorkspaceDialog();

private:
    void applyTitlebarTheme();
    int getTitlebarHeightPixels() const;
    int getResizeBorderWidth() const;

    Application* app_ = nullptr;
    GLFWwindow* window_ = nullptr;

    bool lastAppliedDarkTheme_ = true;
    bool themeChanged_ = false;
    bool titlebarWidgetHovered_ = false;

    bool openWorkspacePopup_ = false;
    bool openMenuPopup_ = false;
    ImVec2 workspacePopupPos_ = {};
    ImVec2 menuPopupPos_ = {};

    // Interactive regions in screen coords — used by hitTest()
    float interactiveLeftEnd_ = 0.0f;
    float interactiveRightStart_ = 0.0f;
};

#endif
