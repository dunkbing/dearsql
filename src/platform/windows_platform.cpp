#if defined(_WIN32)

#include "platform/windows_platform.hpp"
#include "application.hpp"
#include "imgui_impl_glfw.h"
#include "themes.hpp"

#include <dwmapi.h>
#include <iostream>
#include <windowsx.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

WNDPROC WindowsPlatform::originalWndProc_ = nullptr;
WindowsPlatform* WindowsPlatform::instance_ = nullptr;

LRESULT CALLBACK WindowsPlatform::customWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // let DWM handle its messages first (shadow, etc.)
    LRESULT dwmResult = 0;
    if (DwmDefWindowProc(hWnd, msg, wParam, lParam, &dwmResult)) {
        return dwmResult;
    }

    switch (msg) {
    case WM_ACTIVATE: {
        // extend DWM frame by 1px at top for the window shadow effect
        MARGINS margins = {0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        return 0;
    }

    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            // return 0 to remove the standard non-client frame.
            // when maximized, the OS extends the window beyond the screen by the
            // frame thickness. compensate so the window doesn't cover the taskbar.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(hWnd)) {
                HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {sizeof(mi)};
                if (GetMonitorInfoW(monitor, &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0;
        }
        break;
    }

    case WM_NCHITTEST: {
        if (instance_ && instance_->titlebar_) {
            LRESULT hit = instance_->titlebar_->hitTest(hWnd, lParam);
            if (hit != HTNOWHERE) {
                return hit;
            }
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        // ensure maximized window fits in the work area (excludes taskbar)
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(monitor, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }

    case WM_SIZE: {
        // resize D3D11 swap chain when the window is resized
        if (wParam != SIZE_MINIMIZED && instance_ && instance_->backend_) {
            instance_->backend_->resizeSwapChain(LOWORD(lParam), HIWORD(lParam));
        }
        break; // also let GLFW process this
    }
    }

    return CallWindowProcW(originalWndProc_, hWnd, msg, wParam, lParam);
}

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

    backend_ = std::make_unique<WindowsDX11Backend>(hWnd);

    std::cout << "DirectX 11 device initialized successfully" << std::endl;

    // install custom frame (subclass the HWND created by GLFW)
    subclassWindow();

    glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
        for (int i = 0; i < count; i++) {
            Application::getInstance().openFile(std::string(paths[i]));
        }
        glfwFocusWindow(w);
    });

    return true;
}

void WindowsPlatform::subclassWindow() {
    HWND hWnd = getHWND();
    if (!hWnd)
        return;

    instance_ = this;
    originalWndProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(customWndProc)));

    // extend DWM frame (1px top for shadow)
    MARGINS margins = {0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hWnd, &margins);

    // force a WM_NCCALCSIZE so the custom frame takes effect immediately
    RECT rc;
    GetWindowRect(hWnd, &rc);
    SetWindowPos(hWnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool WindowsPlatform::initializeImGuiBackend() {
    ImGui_ImplGlfw_InitForOther(window_, true);
    return backend_->initializeImGui();
}

void WindowsPlatform::setupTitlebar() {
    titlebar_ = std::make_unique<WindowsTitlebar>(app_, window_);
    titlebar_->setup();
}

float WindowsPlatform::getTitlebarHeight() const {
    return titlebar_ ? titlebar_->getHeight() : 0.0f;
}

float WindowsPlatform::getClientAreaTopInset() const {
    return titlebar_ ? titlebar_->getClientAreaTopInset() : 0.0f;
}

void WindowsPlatform::onSidebarToggleClicked() {
    if (app_) {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    }
}

void WindowsPlatform::cleanup() {
    backend_.reset();
}

void WindowsPlatform::renderFrame() {
    if (!backend_)
        return;

    // check theme change and re-apply DWM colour if needed
    if (titlebar_ && titlebar_->themeChanged()) {
        titlebar_->clearThemeChanged();
        titlebar_->applyTheme(app_->isDarkTheme());
    }

    const auto& clearCol = app_->isDarkTheme() ? Theme::NATIVE_DARK.base : Theme::NATIVE_LIGHT.base;
    backend_->beginFrame(clearCol);

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (titlebar_) {
        titlebar_->render();
        titlebar_->renderPopups();
    }
    app_->renderMainUI();

    ImGui::Render();

    backend_->renderDrawData(ImGui::GetDrawData());
    backend_->present();
}

void WindowsPlatform::shutdownImGui() {
    backend_->shutdown();
    ImGui_ImplGlfw_Shutdown();
}

HWND WindowsPlatform::getHWND() const {
    if (!window_) {
        return nullptr;
    }
    return glfwGetWin32Window(window_);
}

ImTextureID WindowsPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    return backend_ ? backend_->createTextureFromRGBA(pixels, width, height) : ImTextureID{};
}

#endif
