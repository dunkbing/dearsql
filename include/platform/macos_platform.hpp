#pragma once

#include "platform_interface.hpp"
#include <memory>

#ifdef USE_METAL_BACKEND
// Forward declarations for Objective-C types
#ifdef __OBJC__
@class ToolbarDelegate;
#else
typedef void ToolbarDelegate;
#endif
#endif

class Application;

class MacOSPlatform final : public PlatformInterface {
public:
    MacOSPlatform(Application* app);
    ~MacOSPlatform() override;

    bool initializePlatform(GLFWwindow* window) override;
    bool initializeImGuiBackend() override;
    void setupTitlebar() override;
    float getTitlebarHeight() const override;
    void onSidebarToggleClicked() override;
    void onLogPanelToggleClicked() override;
    void cleanup() override;
    void renderFrame() override;
    void shutdownImGui() override;
    void updateWorkspaceDropdown() override;

private:
    Application* app_;
    GLFWwindow* window_;

#ifdef USE_METAL_BACKEND
    ToolbarDelegate* toolbarDelegate_;
#ifdef __OBJC__
    id metalDevice_;
    id metalCommandQueue_;
    id metalLayer_;
#else
    void* metalDevice_;
    void* metalCommandQueue_;
    void* metalLayer_;
#endif
#endif
};
