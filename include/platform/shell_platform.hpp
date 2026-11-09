#pragma once

#include "mydear/app.hpp"
#include "platform/titlebar.hpp"
#include "platform_interface.hpp"
#include <memory>

#if defined(_WIN32)
using ShellTitlebar = WindowsTitlebar;
#elif defined(__linux__)
using ShellTitlebar = LinuxTitlebar;
#else
using ShellTitlebar = MacOSTitlebar;
#endif

class Application;

// macos, windows and linux: mydear owns the window, the graphics backend and the loop; this adds
// the title bar (ui/title_bar.cpp in mydear's strip) and the native dialogs behind it
class ShellPlatform final : public PlatformInterface {
public:
    ShellPlatform(Application* app, mydear::App& shell);
    ~ShellPlatform() override;

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
    std::unique_ptr<ShellTitlebar> titlebar_;
};
