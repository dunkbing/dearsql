#pragma once

// Forward declare GLFWwindow since we don't use it on Linux
struct GLFWwindow;

#include "imgui.h"
#include "platform_interface.hpp"
#include <gtk/gtk.h>

class Application;
struct Workspace;

class LinuxPlatform final : public PlatformInterface {
public:
    explicit LinuxPlatform(Application* app);
    ~LinuxPlatform() override;

    bool initializePlatform(GLFWwindow* window) override;
    bool initializeImGuiBackend() override;
    void setupTitlebar() override;
    float getTitlebarHeight() const override;
    void onSidebarToggleClicked() override;
    void cleanup() override;
    void renderFrame() override;
    void shutdownImGui() override;
    void updateWorkspaceDropdown() override;

    // GTK-specific methods
    bool initializeGTK(int* argc, char*** argv);
    void runMainLoop();
    bool shouldClose() const;
    void getFramebufferSize(int* width, int* height) const;
    void swapBuffers();
    void pollEvents();

    // Callbacks
    static gboolean onRender(GtkGLArea* area, GdkGLContext* context, gpointer userData);
    static void onRealize(GtkGLArea* area, gpointer userData);
    static void onResize(GtkGLArea* area, gint width, gint height, gpointer userData);
    static gboolean onKeyPress(GtkEventControllerKey* controller, guint keyval, guint keycode,
                               GdkModifierType state, gpointer userData);
    static gboolean onKeyRelease(GtkEventControllerKey* controller, guint keyval, guint keycode,
                                 GdkModifierType state, gpointer userData);
    static void onMotionNotify(GtkEventControllerMotion* controller, gdouble x, gdouble y,
                               gpointer userData);
    static void onButtonPress(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y,
                              gpointer userData);
    static void onButtonRelease(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y,
                                gpointer userData);
    static gboolean onScroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy,
                             gpointer userData);
    static void onSidebarToggle(GtkButton* button, gpointer userData);
    static void onWorkspaceChanged(GtkDropDown* dropdown, GParamSpec* pspec, gpointer userData);
    static void onAddConnection(GtkButton* button, gpointer userData);
    static gboolean onClose(GtkWindow* window, gpointer userData);

    // Menu callbacks
    static void onMenuButtonClicked(GtkButton* button, gpointer userData);
    static void onThemeLightClicked(GtkButton* button, gpointer userData);
    static void onThemeDarkClicked(GtkButton* button, gpointer userData);
    static void onThemeAutoClicked(GtkButton* button, gpointer userData);
    static void onLicenseClicked(GtkButton* button, gpointer userData);

    void showLicenseDialog();

private:
    Application* app_;
    GtkWidget* window_;
    GtkWidget* glArea_;
    GtkWidget* headerBar_;
    GtkWidget* sidebarButton_;
    GtkWidget* workspaceDropdown_;
    GtkWidget* addButton_;
    GtkWidget* menuButton_;
    GtkWidget* menuPopover_;
    GtkWidget* themeLightButton_;
    GtkWidget* themeDarkButton_;
    GtkWidget* themeAutoButton_;
    GtkWidget* licenseButton_;
    GtkStringList* workspaceModel_;

    void updateThemeButtons();
    void updateLicenseButton();

    bool shouldClose_;
    bool realized_;
    int fbWidth_;
    int fbHeight_;
    double mouseX_;
    double mouseY_;

    void setupInputHandlers();
    void updateImGuiMousePos();
    void updateImGuiKeyMods(GdkModifierType state);
    ImGuiKey gtkKeyToImGuiKey(guint keyval);
};
