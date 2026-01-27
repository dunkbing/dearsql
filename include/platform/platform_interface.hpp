#pragma once

#if defined(__APPLE__) || defined(_WIN32)
#include <GLFW/glfw3.h>
#else
// Forward declare for Linux (uses GTK instead of GLFW)
struct GLFWwindow;
#endif

class PlatformInterface {
public:
    virtual ~PlatformInterface() = default;

    virtual bool initializePlatform(GLFWwindow* window) = 0;
    virtual bool initializeImGuiBackend() = 0;
    virtual void setupTitlebar() = 0;
    virtual float getTitlebarHeight() const = 0;
    virtual void onSidebarToggleClicked() = 0;
    virtual void cleanup() = 0;
    virtual void renderFrame() = 0;
    virtual void shutdownImGui() = 0;
    virtual void updateWorkspaceDropdown() {}
};
