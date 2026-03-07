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

    // public accessors for alert/dialog use
    [[nodiscard]] HWND getHWND() const;

private:
    static LRESULT CALLBACK TitlebarWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createTitlebarControls(HWND hWnd);
    void destroyTitlebarControls();
    void layoutTitlebarControls();
    void showCreateWorkspaceDialog();
    LRESULT handleWindowMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled);
    bool createD3DDevice(HWND hWnd);
    void cleanupD3DDevice();
    void createRenderTarget();
    void cleanupRenderTarget();
    void applyTitlebarTheme();

    Application* app_;
    GLFWwindow* window_ = nullptr;

    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dDeviceContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView_ = nullptr;
    void* oldWndProc_ = nullptr;
    HWND sidebarButton_ = nullptr;
    HWND addButton_ = nullptr;
    HWND workspaceDropdown_ = nullptr;
    bool updatingWorkspaceDropdown_ = false;
    bool lastAppliedDarkTheme_ = true;
    int titlebarHeightPx_ = 40;
    int newWorkspaceItemIndex_ = -1;
    std::vector<int> workspaceIdsByIndex_;
};

#endif
