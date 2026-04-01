#pragma once

#include "imgui.h"
#include <cstdint>

// Abstract graphics backend — one platform implementation each
class IGraphicsBackend {
public:
    virtual ~IGraphicsBackend() = default;

    // Initialize the ImGui rendering backend (OpenGL/Metal/DX11)
    virtual bool initializeImGui() = 0;

    // Start a frame: activate context, clear buffers, call ImGui_Impl*_NewFrame()
    virtual void beginFrame(ImVec4 clearColor) = 0;

    // Render ImGui draw data to the back buffer
    virtual void renderDrawData(ImDrawData* drawData) = 0;

    // Present / swap the finished frame
    virtual void present() = 0;

    // Handle framebuffer resize
    virtual void onResize(int width, int height) = 0;

    // Shutdown the ImGui rendering backend
    virtual void shutdown() = 0;

    // Return the current framebuffer dimensions
    virtual void getFramebufferSize(int& width, int& height) = 0;

    // Upload RGBA pixel data to a GPU texture; returns 0 on failure
    virtual ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) = 0;
};

// ---- Linux: OpenGL 3.3 via GTK GtkGLArea ----
#if defined(__linux__)

#include <gtk/gtk.h>

class LinuxOpenGLBackend final : public IGraphicsBackend {
public:
    LinuxOpenGLBackend();

    // The platform adds this widget to the window layout
    GtkWidget* getGLArea() const {
        return glArea_;
    }

    bool initializeImGui() override;
    void beginFrame(ImVec4 clearColor) override;
    void renderDrawData(ImDrawData* drawData) override;
    // GTK presents automatically after onRender returns
    void present() override {}
    void onResize(int width, int height) override;
    void shutdown() override;
    void getFramebufferSize(int& width, int& height) override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

private:
    GtkWidget* glArea_ = nullptr;
    int fbWidth_ = 1280;
    int fbHeight_ = 720;
};

// ---- macOS: Metal via CAMetalLayer (GLFW window) ----
#elif defined(__APPLE__)

#include <GLFW/glfw3.h>

class MacOSMetalBackend final : public IGraphicsBackend {
public:
    explicit MacOSMetalBackend(GLFWwindow* window);

    bool initializeImGui() override;
    // beginFrame acquires the drawable and stores frame-local Metal objects
    void beginFrame(ImVec4 clearColor) override;
    void renderDrawData(ImDrawData* drawData) override;
    void present() override;
    void onResize(int width, int height) override {}
    void shutdown() override;
    void getFramebufferSize(int& width, int& height) override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

#ifdef __OBJC__
    id metalDevice_ = nullptr;
    id metalCommandQueue_ = nullptr;
    id metalLayer_ = nullptr;
    // per-frame objects — valid between beginFrame() and present()
    id currentDrawable_ = nullptr;
    id currentCommandBuffer_ = nullptr;
    id currentRenderEncoder_ = nullptr;
#else
    void* metalDevice_ = nullptr;
    void* metalCommandQueue_ = nullptr;
    void* metalLayer_ = nullptr;
    void* currentDrawable_ = nullptr;
    void* currentCommandBuffer_ = nullptr;
    void* currentRenderEncoder_ = nullptr;
#endif

private:
    GLFWwindow* window_ = nullptr;
};

// ---- Windows: Direct3D 11 via GLFW/HWND ----
#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;

struct GLFWwindow;

class WindowsDX11Backend final : public IGraphicsBackend {
public:
    explicit WindowsDX11Backend(HWND hwnd);
    ~WindowsDX11Backend();

    bool initializeImGui() override;
    void beginFrame(ImVec4 clearColor) override;
    void renderDrawData(ImDrawData* drawData) override;
    void present() override;
    void onResize(int width, int height) override;
    void shutdown() override;
    void getFramebufferSize(int& width, int& height) override;
    ImTextureID createTextureFromRGBA(const uint8_t* pixels, int width, int height) override;

    // Called from WndProc WM_SIZE to resize the swap chain
    void resizeSwapChain(int width, int height);

private:
    bool createD3DDevice();
    void cleanupD3DDevice();
    void createRenderTarget();
    void cleanupRenderTarget();

    HWND hwnd_ = nullptr;
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dDeviceContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

#endif
