#pragma once

#include "mydear/app.hpp"
#include "platform/titlebar.hpp"
#include "platform_interface.hpp"
#include <memory>

class Application;

// macos: mydear owns the window, metal and the loop; this adds the toolbar title bar
class MacOSPlatform final : public PlatformInterface {
public:
    MacOSPlatform(Application* app, mydear::App& shell);
    ~MacOSPlatform() override;

    bool initializePlatform(GLFWwindow*) override {
        return true;
    }
    bool initializeImGuiBackend() override {
        return true;
    }
    void setupTitlebar() override;
    float getTitlebarHeight() const override;
    float getClientAreaTopInset() const override {
        return titlebar_ ? titlebar_->getClientAreaTopInset() : 0.0f;
    }
    void onSidebarToggleClicked() override;
    void cleanup() override;
    void renderFrame() override {
        shell_.renderFrame();
    }
    void renderChrome() override;
    void setVSync(bool on) override {
        shell_.setVSync(on);
    }
    void shutdownImGui() override {}
    void updateWorkspaceDropdown() override;
    void applyTheme(bool isDark) override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override {
        return shell_.createTexture(pixels, width, height);
    }

private:
    Application* app_;
    mydear::App& shell_;
    std::unique_ptr<MacOSTitlebar> titlebar_;
};
