#if defined(_WIN32)

#include "platform/windows_platform.hpp"
#include "application.hpp"
#include "imgui_impl_dx11.h"
#include "imgui_impl_glfw.h"
#include "themes.hpp"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <iostream>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

WindowsPlatform::WindowsPlatform(Application* app) : app_(app) {}

WindowsPlatform::~WindowsPlatform() {
    cleanup();
}

bool WindowsPlatform::initializePlatform(GLFWwindow* window) {
    window_ = window;

    HWND hWnd = glfwGetWin32Window(window);
    if (!hWnd) {
        std::cerr << "Failed to get Win32 window handle from GLFW" << std::endl;
        return false;
    }

    if (!createD3DDevice(hWnd)) {
        std::cerr << "Failed to create D3D11 device" << std::endl;
        return false;
    }

    std::cout << "DirectX 11 device initialized successfully" << std::endl;
    return true;
}

bool WindowsPlatform::initializeImGuiBackend() {
    if (!d3dDevice_ || !d3dDeviceContext_) {
        return false;
    }

    ImGui_ImplDX11_Init(d3dDevice_, d3dDeviceContext_);
    std::cout << "ImGui DirectX 11 backend initialized" << std::endl;
    return true;
}

void WindowsPlatform::setupTitlebar() {
    applyTitlebarTheme();
    std::cout << "Windows titlebar configured" << std::endl;
}

float WindowsPlatform::getTitlebarHeight() const {
    return 0.0f; // standard Win32 title bar
}

void WindowsPlatform::onSidebarToggleClicked() {
    if (app_) {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    }
}

void WindowsPlatform::cleanup() {
    cleanupD3DDevice();
}

void WindowsPlatform::renderFrame() {
    if (!d3dDevice_ || !swapChain_ || !mainRenderTargetView_) {
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    app_->renderMainUI();

    ImGui::Render();

    const auto& clearCol = app_->isDarkTheme() ? Theme::NATIVE_DARK.base : Theme::NATIVE_LIGHT.base;
    const float clearColor[4] = {clearCol.x, clearCol.y, clearCol.z, clearCol.w};
    d3dDeviceContext_->OMSetRenderTargets(1, &mainRenderTargetView_, nullptr);
    d3dDeviceContext_->ClearRenderTargetView(mainRenderTargetView_, clearColor);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // vsync
    swapChain_->Present(1, 0);
}

void WindowsPlatform::shutdownImGui() {
    ImGui_ImplDX11_Shutdown();
    std::cout << "ImGui DirectX 11 backend shutdown" << std::endl;
}

void WindowsPlatform::updateWorkspaceDropdown() {
    // workspace dropdown is rendered via ImGui sidebar on Windows
}

HWND WindowsPlatform::getHWND() const {
    if (!window_) {
        return nullptr;
    }
    return glfwGetWin32Window(window_);
}

bool WindowsPlatform::createD3DDevice(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    constexpr D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &swapChain_, &d3dDevice_, &featureLevel, &d3dDeviceContext_);

    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDeviceAndSwapChain failed: 0x" << std::hex << hr << std::dec
                  << std::endl;
        return false;
    }

    createRenderTarget();
    return true;
}

void WindowsPlatform::cleanupD3DDevice() {
    cleanupRenderTarget();
    if (swapChain_) {
        swapChain_->Release();
        swapChain_ = nullptr;
    }
    if (d3dDeviceContext_) {
        d3dDeviceContext_->Release();
        d3dDeviceContext_ = nullptr;
    }
    if (d3dDevice_) {
        d3dDevice_->Release();
        d3dDevice_ = nullptr;
    }
}

void WindowsPlatform::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        d3dDevice_->CreateRenderTargetView(backBuffer, nullptr, &mainRenderTargetView_);
        backBuffer->Release();
    }
}

void WindowsPlatform::cleanupRenderTarget() {
    if (mainRenderTargetView_) {
        mainRenderTargetView_->Release();
        mainRenderTargetView_ = nullptr;
    }
}

void WindowsPlatform::applyTitlebarTheme() {
    HWND hWnd = getHWND();
    if (!hWnd || !app_) {
        return;
    }

    bool isDark = app_->isDarkTheme();

    // set dark mode for title bar (Windows 10 1809+)
    BOOL useDarkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    // set caption color (Windows 11+, silently ignored on older)
    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    COLORREF captionColor =
        RGB(static_cast<int>(colors.mantle.x * 255), static_cast<int>(colors.mantle.y * 255),
            static_cast<int>(colors.mantle.z * 255));
    DwmSetWindowAttribute(hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
}

ImTextureID WindowsPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    if (!d3dDevice_ || !pixels) {
        return ImTextureID{};
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = pixels;
    subResource.SysMemPitch = width * 4;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = d3dDevice_->CreateTexture2D(&desc, &subResource, &texture);
    if (FAILED(hr)) {
        return ImTextureID{};
    }

    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = d3dDevice_->CreateShaderResourceView(texture, &srvDesc, &srv);
    texture->Release();

    if (FAILED(hr)) {
        return ImTextureID{};
    }

    return (ImTextureID)(intptr_t)srv;
}

#endif
