#pragma once

#if defined(_WIN32)

#include "platform_interface.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <vector>
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;

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
    void updateWorkspaceDropdown() override;
    [[nodiscard]] float getClientAreaTopInset() const override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

    [[nodiscard]] HWND getHWND() const;

private:
    // Stubs retained for interface compatibility
    void createTitlebarControls(HWND hWnd);
    void destroyTitlebarControls();
    void layoutTitlebarControls();
    void showCreateWorkspaceDialog();
    LRESULT handleWindowMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled);

    // D3D11
    bool createD3DDevice(HWND hWnd);
    void cleanupD3DDevice();
    void createRenderTarget();
    void cleanupRenderTarget();

    // License dialog
    void showLicenseDialog();

    // Custom frame
    void subclassWindow();
    void applyTitlebarTheme();
    void renderTitlebar();
    void renderTitlebarPopups();
    [[nodiscard]] int getTitlebarHeightPixels() const;
    [[nodiscard]] int getResizeBorderWidth() const;
    LRESULT hitTest(HWND hWnd, LPARAM lParam) const;

    static LRESULT CALLBACK customWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static WNDPROC originalWndProc_;
    static WindowsPlatform* instance_;

    Application* app_;
    GLFWwindow* window_ = nullptr;

    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dDeviceContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView_ = nullptr;
    bool lastAppliedDarkTheme_ = true;
    bool titlebarWidgetHovered_ = false;

    // Popup state
    bool openWorkspacePopup_ = false;
    bool openMenuPopup_ = false;
    ImVec2 workspacePopupPos_ = {};
    ImVec2 menuPopupPos_ = {};

    // Interactive regions (screen coords) for hit testing
    float interactiveLeftEnd_ = 0;
    float interactiveRightStart_ = 0;
};

#endif
