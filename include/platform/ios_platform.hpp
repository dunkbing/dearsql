#pragma once

#include "platform/platform_interface.hpp"

class Application;

// tag of the imgui view (the file picker pauses it)
inline constexpr long kDearSQLViewTag = 0xDEA5;

// ipad: a uikit scene with one MTKView; the view's draw callback runs renderFrame()
class IOSPlatform : public PlatformInterface {
public:
    explicit IOSPlatform(Application* app) : app_(app) {}

    bool initializePlatform(GLFWwindow*) override {
        return true;
    }
    bool initializeImGuiBackend() override;
    void setupTitlebar() override {}
    [[nodiscard]] float getTitlebarHeight() const override {
        return 0.0f;
    }
    void onSidebarToggleClicked() override;
    void cleanup() override {}
    void renderFrame() override;
    void shutdownImGui() override;
    void applyTheme(bool dark) override; // the navigation bar
    // the status bar / safe area at the top of the screen
    [[nodiscard]] float getClientAreaTopInset() const override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

private:
    Application* app_;
};
