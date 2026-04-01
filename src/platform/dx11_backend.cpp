#if defined(_WIN32)

#include "imgui_impl_dx11.h"
#include "platform/graphics_backend.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <iostream>

// ---------------------------------------------------------------------------
// WindowsDX11Backend
// ---------------------------------------------------------------------------

WindowsDX11Backend::WindowsDX11Backend(HWND hwnd) : hwnd_(hwnd) {
    createD3DDevice();
}

WindowsDX11Backend::~WindowsDX11Backend() {
    cleanupD3DDevice();
}

bool WindowsDX11Backend::createD3DDevice() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    constexpr D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &swapChain_, &d3dDevice_, &featureLevel, &d3dDeviceContext_);

    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDeviceAndSwapChain failed: 0x" << std::hex << hr << std::dec
                  << std::endl;
        return false;
    }

    createRenderTarget();
    return true;
}

void WindowsDX11Backend::cleanupD3DDevice() {
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

void WindowsDX11Backend::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        d3dDevice_->CreateRenderTargetView(backBuffer, nullptr, &mainRenderTargetView_);
        backBuffer->Release();
    }
}

void WindowsDX11Backend::cleanupRenderTarget() {
    if (mainRenderTargetView_) {
        mainRenderTargetView_->Release();
        mainRenderTargetView_ = nullptr;
    }
}

bool WindowsDX11Backend::initializeImGui() {
    if (!d3dDevice_ || !d3dDeviceContext_) {
        return false;
    }
    ImGui_ImplDX11_Init(d3dDevice_, d3dDeviceContext_);
    std::cout << "ImGui DirectX 11 backend initialized" << std::endl;
    return true;
}

void WindowsDX11Backend::beginFrame(ImVec4 clearColor) {
    ImGui_ImplDX11_NewFrame();

    const float clearColorF[4] = {clearColor.x, clearColor.y, clearColor.z, clearColor.w};
    d3dDeviceContext_->OMSetRenderTargets(1, &mainRenderTargetView_, nullptr);
    d3dDeviceContext_->ClearRenderTargetView(mainRenderTargetView_, clearColorF);
}

void WindowsDX11Backend::renderDrawData(ImDrawData* drawData) {
    ImGui_ImplDX11_RenderDrawData(drawData);
}

void WindowsDX11Backend::present() {
    swapChain_->Present(1, 0);
}

void WindowsDX11Backend::onResize(int width, int height) {
    resizeSwapChain(width, height);
}

void WindowsDX11Backend::resizeSwapChain(int width, int height) {
    cleanupRenderTarget();
    swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
    width_ = width;
    height_ = height;
}

void WindowsDX11Backend::shutdown() {
    ImGui_ImplDX11_Shutdown();
    std::cout << "ImGui DirectX 11 backend shutdown" << std::endl;
}

void WindowsDX11Backend::getFramebufferSize(int& width, int& height) {
    width = width_;
    height = height_;
}

ImTextureID WindowsDX11Backend::createTextureFromRGBA(const uint8_t* pixels, int width,
                                                      int height) {
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

    return (ImTextureID)(INT_PTR)srv;
}

#endif
