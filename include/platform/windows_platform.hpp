#pragma once

#if defined(_WIN32)

#include "platform_interface.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct HWND__;
typedef HWND__* HWND;

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

    // public accessors for alert/dialog use
    [[nodiscard]] HWND getHWND() const;

private:
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
};

#endif
