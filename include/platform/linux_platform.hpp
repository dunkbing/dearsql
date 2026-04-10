#pragma once

#if defined(__linux__)

// Forward declare GLFWwindow since we don't use it on Linux
struct GLFWwindow;

#include "imgui.h"
#include "platform/graphics_backend.hpp"
#include "platform/titlebar.hpp"
#include "platform_interface.hpp"
#include <gtk/gtk.h>
#include <memory>

class Application;

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
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;
    void applyCurrentTheme();

    // GTK-specific methods
    bool initializeGTK(int* argc, char*** argv);
    void runMainLoop();
    bool shouldClose() const;
    void getFramebufferSize(int* width, int* height) const;
    void pollEvents();

    [[nodiscard]] GtkWidget* getGtkWindow() const {
        return window_;
    }

    // GL area signal callbacks (static — userData is LinuxPlatform*)
    static gboolean onRender(GtkGLArea* area, GdkGLContext* context, gpointer userData);
    static void onRealize(GtkGLArea* area, gpointer userData);
    static void onResize(GtkGLArea* area, gint width, gint height, gpointer userData);
    static gboolean onTickCallback(GtkWidget* widget, GdkFrameClock* clock, gpointer userData);

    // Input callbacks (static — userData is LinuxPlatform*)
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
    static gboolean onDrop(GtkDropTarget* target, const GValue* value, double x, double y,
                           gpointer userData);
    static gboolean onClose(GtkWindow* window, gpointer userData);

    void noteInteraction();

private:
    Application* app_;
    GtkWidget* window_;

    std::unique_ptr<LinuxOpenGLBackend> backend_;
    std::unique_ptr<LinuxTitlebar> titlebar_;

    bool shouldClose_;
    double mouseX_;
    double mouseY_;
    ImGuiMouseCursor lastCursor_ = ImGuiMouseCursor_COUNT;

    void setupInputHandlers();
    void updateImGuiMousePos();
    void updateImGuiKeyMods(GdkModifierType state);
    ImGuiKey gtkKeyToImGuiKey(guint keyval);

    gint64 lastInteractionTimeUs_ = 0;
    bool lastHadAsyncWork_ = false;
    bool lastWindowFocused_ = false;
    guint tickCallbackId_ = 0;

    void ensureTickCallback();
};

#endif // defined(__linux__)
