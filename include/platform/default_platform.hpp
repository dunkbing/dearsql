#pragma once

#include "platform_interface.hpp"

class Application;

class DefaultPlatform final : public PlatformInterface {
public:
    explicit DefaultPlatform(Application *app);
    ~DefaultPlatform() override = default;

    bool initializePlatform(GLFWwindow *window) override;
    bool initializeImGuiBackend() override;
    void setupTitlebar() override;
    float getTitlebarHeight() const override;
    void onConnectButtonClicked() override;
    void cleanup() override;
    void renderFrame() override;
    void shutdownImGui() override;

private:
    Application *app_;
    GLFWwindow *window_;
};
