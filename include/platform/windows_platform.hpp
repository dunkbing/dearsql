#pragma once

#if defined(_WIN32)

#include "graphics_backend.hpp"
#include "platform_interface.hpp"
#include "titlebar.hpp"
#include <memory>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class Application;

class WindowsPlatform final : public PlatformInterface {
public:
    explicit WindowsPlatform(Application* app);
    ~WindowsPlatform() override;

    bool initializePlatform(GLFWwindow* window) override;
    bool initializeImGuiBackend() override;
    void setupTitlebar() override;
    float getTitlebarHeight() const override;
    void onSidebarToggleClicked() override;
    void cleanup() override;
    void renderFrame() override;
    void shutdownImGui() override;
    void updateWorkspaceDropdown() override {}
    [[nodiscard]] float getClientAreaTopInset() const override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;
    [[nodiscard]] HWND getHWND() const;

private:
    void subclassWindow();
    static LRESULT CALLBACK customWndProc(HWND, UINT, WPARAM, LPARAM);
    static WNDPROC originalWndProc_;
    static WindowsPlatform* instance_;

    Application* app_;
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<WindowsDX11Backend> backend_;
    std::unique_ptr<WindowsTitlebar> titlebar_;
};

#endif
