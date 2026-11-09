#pragma once

#include "mydear/app.hpp"
#include "platform/platform_interface.hpp"

class Application;

// tag of the imgui view (the file picker pauses it)
inline constexpr long kDearSQLViewTag = 0xDEA5;

// ipad: mydear runs the uikit view and loop (mydear/ios.h); this adds the navigation bar,
// whose items live on DearSQLViewController, and the screenshot hooks
class IOSPlatform : public PlatformInterface {
public:
    IOSPlatform(Application* app, mydear::App& shell);

    bool initializePlatform(GLFWwindow*) override {
        return true;
    }
    bool initializeImGuiBackend() override {
        return true;
    }
    void setupTitlebar() override {}
    [[nodiscard]] float getTitlebarHeight() const override {
        return 0.0f;
    }
    void onSidebarToggleClicked() override;
    void cleanup() override {}
    void renderFrame() override;
    void renderChrome() override; // DEARSQL_SHOW
    void shutdownImGui() override {}
    void applyTheme(bool dark) override; // the navigation bar
    // the status bar / safe area at the top of the screen
    [[nodiscard]] float getClientAreaTopInset() const override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

private:
    Application* app_;
    mydear::App& shell_;
};
