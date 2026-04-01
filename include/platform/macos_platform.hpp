#pragma once

#include "platform/graphics_backend.hpp"
#include "platform/titlebar.hpp"
#include "platform_interface.hpp"
#include <memory>

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
    void cleanup() override;
    void renderFrame() override;
    void shutdownImGui() override;
    void updateWorkspaceDropdown() override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

private:
    Application* app_;
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<MacOSMetalBackend> backend_;
    std::unique_ptr<MacOSTitlebar> titlebar_;
};
