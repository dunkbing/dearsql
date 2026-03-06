#pragma once

#include "platform_interface.hpp"

class Application;

class DefaultPlatform final : public PlatformInterface {
public:
    explicit DefaultPlatform(Application* app);
    ~DefaultPlatform() override = default;

    bool initializePlatform(GLFWwindow* window) override;
    bool initializeImGuiBackend() override;
    void setupTitlebar() override;
    float getTitlebarHeight() const override;
    void onSidebarToggleClicked() override;
    void cleanup() override;
    void renderFrame() override;
    void shutdownImGui() override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

private:
    Application* app_;
    GLFWwindow* window_;
};
