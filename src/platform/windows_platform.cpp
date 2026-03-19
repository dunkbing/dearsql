#if defined(_WIN32)

#include "platform/windows_platform.hpp"
#include "application.hpp"
#include "config.hpp"
#include "imgui_impl_dx11.h"
#include "imgui_impl_glfw.h"
#include "license/license_manager.hpp"
#include "themes.hpp"

#include "IconsFontAwesome6.h"

#include <d3d11.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <iostream>
#include <string>
#include <windowsx.h>
#include <commctrl.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif


// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

WNDPROC WindowsPlatform::originalWndProc_ = nullptr;
WindowsPlatform* WindowsPlatform::instance_ = nullptr;

// ---------------------------------------------------------------------------
// Custom WndProc for DWM custom frame
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowsPlatform::customWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Let DWM handle its messages first (shadow, etc.)
    LRESULT dwmResult = 0;
    if (DwmDefWindowProc(hWnd, msg, wParam, lParam, &dwmResult)) {
        return dwmResult;
    }

    switch (msg) {
    case WM_ACTIVATE: {
        // Extend DWM frame by 1px at top for the window shadow effect
        MARGINS margins = {0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        return 0;
    }

    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            // Return 0 to remove the standard non-client frame.
            // When maximized, the OS extends the window beyond the screen by the
            // frame thickness. Compensate so the window doesn't cover the taskbar.
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
        if (instance_) {
            LRESULT hit = instance_->hitTest(hWnd, lParam);
            if (hit != HTNOWHERE) {
                return hit;
            }
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        // Ensure maximized window fits in the work area (excludes taskbar).
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
        // Resize D3D11 swap chain when the window is resized.
        if (wParam != SIZE_MINIMIZED && instance_ && instance_->swapChain_) {
            instance_->cleanupRenderTarget();
            instance_->swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            instance_->createRenderTarget();
        }
        break; // Also let GLFW process this
    }
    }

    return CallWindowProcW(originalWndProc_, hWnd, msg, wParam, lParam);
}

LRESULT WindowsPlatform::hitTest(HWND hWnd, LPARAM lParam) const {
    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    RECT rc;
    GetWindowRect(hWnd, &rc);

    const int border = getResizeBorderWidth();
    const int titlebar = getTitlebarHeightPixels();
    const bool maximized = IsZoomed(hWnd);

    // --- Resize borders (disabled when maximized) ---
    if (!maximized) {
        // Top edge
        if (pt.y >= rc.top && pt.y < rc.top + border) {
            if (pt.x < rc.left + border) return HTTOPLEFT;
            if (pt.x >= rc.right - border) return HTTOPRIGHT;
            return HTTOP;
        }
        // Bottom edge
        if (pt.y >= rc.bottom - border) {
            if (pt.x < rc.left + border) return HTBOTTOMLEFT;
            if (pt.x >= rc.right - border) return HTBOTTOMRIGHT;
            return HTBOTTOM;
        }
        // Left edge
        if (pt.x >= rc.left && pt.x < rc.left + border) return HTLEFT;
        // Right edge
        if (pt.x >= rc.right - border) return HTRIGHT;
    }

    // --- Titlebar area ---
    if (pt.y < rc.top + titlebar) {
        // Caption buttons zone (right side: 3 buttons * 46px)
        if (pt.x >= rc.right - 138) {
            return HTCLIENT;
        }
        // Left interactive zone (sidebar toggle, add button, etc.)
        if (pt.x < rc.left + static_cast<int>(interactiveLeftEnd_)) {
            return HTCLIENT;
        }
        // Right interactive zone (workspace dropdown, menu button)
        if (interactiveRightStart_ > 0 && pt.x >= rc.left + static_cast<int>(interactiveRightStart_)) {
            return HTCLIENT;
        }
        return HTCAPTION;
    }

    // Everything else is normal client area.
    return HTCLIENT;
}

int WindowsPlatform::getTitlebarHeightPixels() const {
    return 32;
}

int WindowsPlatform::getResizeBorderWidth() const {
    return GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

// ---------------------------------------------------------------------------
// WindowsPlatform core
// ---------------------------------------------------------------------------

WindowsPlatform::WindowsPlatform(Application* app) : app_(app) {
    lastAppliedDarkTheme_ = app ? app->isDarkTheme() : true;
}

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

    // Install custom frame (subclass the HWND created by GLFW)
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
    if (!hWnd) return;

    instance_ = this;
    originalWndProc_ =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hWnd, GWLP_WNDPROC,
                                                     reinterpret_cast<LONG_PTR>(customWndProc)));

    // Extend DWM frame (1px top for shadow)
    MARGINS margins = {0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hWnd, &margins);

    // Force a WM_NCCALCSIZE so the custom frame takes effect immediately
    RECT rc;
    GetWindowRect(hWnd, &rc);
    SetWindowPos(hWnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    HWND hWnd = getHWND();
    if (!hWnd) {
        return;
    }

    applyTitlebarTheme();
    std::cout << "Windows custom titlebar configured" << std::endl;
}

float WindowsPlatform::getTitlebarHeight() const {
    return static_cast<float>(getTitlebarHeightPixels());
}

float WindowsPlatform::getClientAreaTopInset() const {
    return static_cast<float>(getTitlebarHeightPixels());
}

void WindowsPlatform::onSidebarToggleClicked() {
    if (app_) {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    }
}

void WindowsPlatform::cleanup() {
    cleanupD3DDevice();
}

// ---------------------------------------------------------------------------
// Titlebar rendering (ImGui)
// ---------------------------------------------------------------------------

namespace {

void DrawMinimizeIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddLine({center.x - 5, center.y}, {center.x + 5, center.y}, col, 1.0f);
}

void DrawMaximizeIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddRect({center.x - 5, center.y - 5}, {center.x + 5, center.y + 5}, col, 0, 0, 1.0f);
}

void DrawRestoreIcon(ImDrawList* dl, ImVec2 center, ImU32 col, ImU32 bgCol) {
    const float s = 4.0f, off = 2.0f;
    // Back rectangle (top-right, partially occluded)
    dl->AddRect({center.x - s + off, center.y - s - off},
                {center.x + s + off, center.y + s - off}, col, 0, 0, 1.0f);
    // Front rectangle (bottom-left) — fill to occlude back rect lines
    ImVec2 p1{center.x - s, center.y - s + off};
    ImVec2 p2{center.x + s - off, center.y + s};
    dl->AddRectFilled(p1, p2, bgCol);
    dl->AddRect(p1, p2, col, 0, 0, 1.0f);
}

void DrawCloseIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddLine({center.x - 5, center.y - 5}, {center.x + 5, center.y + 5}, col, 1.0f);
    dl->AddLine({center.x + 5, center.y - 5}, {center.x - 5, center.y + 5}, col, 1.0f);
}

} // namespace

void WindowsPlatform::renderTitlebar() {
    const float tbHeight = static_cast<float>(getTitlebarHeightPixels());
    if (tbHeight <= 0) return;

    const bool isDark = app_->isDarkTheme();
    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    const bool maximized = IsZoomed(getHWND());

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin = vp->Pos;
    const float winW = vp->Size.x;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // --- Background ---
    fg->AddRectFilled(origin, {origin.x + winW, origin.y + tbHeight},
                      ImGui::GetColorU32(colors.mantle));

    titlebarWidgetHovered_ = false;

    const ImVec4 hoverBg = isDark ? ImVec4(1, 1, 1, 0.1f) : ImVec4(0, 0, 0, 0.06f);
    const ImVec4 activeBg = isDark ? ImVec4(1, 1, 1, 0.15f) : ImVec4(0, 0, 0, 0.10f);

    // Helper: manual foreground button (no ImGui window context needed)
    auto fgButton = [&](ImVec2 bMin, ImVec2 bMax, auto drawIcon,
                        ImVec4 hBg, ImVec4 aBg, bool whiteIconOnHover) -> bool {
        bool hovered = (mouse.x >= bMin.x && mouse.x < bMax.x &&
                        mouse.y >= bMin.y && mouse.y < bMax.y);
        bool held = hovered && io.MouseDown[0];
        bool clicked = hovered && io.MouseReleased[0];
        titlebarWidgetHovered_ |= hovered;

        if (held)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(aBg));
        else if (hovered)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(hBg));

        ImVec2 center{(bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f};
        ImVec4 iCol = (hovered && whiteIconOnHover) ? ImVec4(1, 1, 1, 1) : colors.text;
        drawIcon(fg, center, ImGui::GetColorU32(iCol));

        return clicked;
    };

    // Helper: simple icon button (rounded, fixed size)
    const float iconBtnSize = tbHeight - 4.0f;
    auto fgIconBtn = [&](float x, const char* iconText) -> bool {
        float iy = origin.y + (tbHeight - iconBtnSize) * 0.5f;
        ImVec2 bMin{x, iy}, bMax{x + iconBtnSize, iy + iconBtnSize};
        bool hovered = (mouse.x >= bMin.x && mouse.x < bMax.x &&
                        mouse.y >= bMin.y && mouse.y < bMax.y);
        bool held = hovered && io.MouseDown[0];
        bool clicked = hovered && io.MouseReleased[0];
        titlebarWidgetHovered_ |= hovered;

        if (held)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(activeBg), 4.0f);
        else if (hovered)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(hoverBg), 4.0f);

        // Center icon text
        ImVec2 textSize = ImGui::CalcTextSize(iconText);
        fg->AddText({(bMin.x + bMax.x - textSize.x) * 0.5f,
                     (bMin.y + bMax.y - textSize.y) * 0.5f},
                    ImGui::GetColorU32(colors.text), iconText);

        return clicked;
    };

    // ===================== RIGHT SIDE: Caption buttons =====================
    const float captionBtnW = 46.0f;
    const ImVec4 closeHoverBg = ImVec4(0.77f, 0.17f, 0.11f, 1.0f);
    const ImVec4 closeActiveBg = ImVec4(0.67f, 0.14f, 0.09f, 1.0f);

    float rx = origin.x + winW - captionBtnW * 3;

    if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight},
                 DrawMinimizeIcon, hoverBg, activeBg, false))
        ShowWindow(getHWND(), SW_MINIMIZE);
    rx += captionBtnW;

    if (maximized) {
        if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight},
                     [&](ImDrawList* d, ImVec2 c, ImU32 col) {
                         DrawRestoreIcon(d, c, col, ImGui::GetColorU32(colors.mantle));
                     },
                     hoverBg, activeBg, false))
            ShowWindow(getHWND(), SW_RESTORE);
    } else {
        if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight},
                     DrawMaximizeIcon, hoverBg, activeBg, false))
            ShowWindow(getHWND(), SW_MAXIMIZE);
    }
    rx += captionBtnW;

    if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight},
                 DrawCloseIcon, closeHoverBg, closeActiveBg, true))
        PostMessageW(getHWND(), WM_CLOSE, 0, 0);

    // ===================== RIGHT SIDE: Menu + Workspace (before caption) =====================
    const float rightGroupX = origin.x + winW - captionBtnW * 3 - 8.0f; // 8px gap before caption btns

    // Menu button (rightmost before caption buttons)
    float menuX = rightGroupX - iconBtnSize;
    if (fgIconBtn(menuX, ICON_FA_ELLIPSIS_VERTICAL)) {
        openMenuPopup_ = true;
        menuPopupPos_ = {menuX, origin.y + tbHeight};
    }

    // Workspace dropdown
    const std::string wsName = app_->getCurrentWorkspaceName();
    const std::string wsLabel = wsName + "  " ICON_FA_CHEVRON_DOWN;
    ImVec2 wsTextSize = ImGui::CalcTextSize(wsLabel.c_str());
    float wsBtnW = wsTextSize.x + 16.0f;
    float wsX = menuX - 4.0f - wsBtnW;
    {
        float iy = origin.y + (tbHeight - iconBtnSize) * 0.5f;
        ImVec2 bMin{wsX, iy}, bMax{wsX + wsBtnW, iy + iconBtnSize};
        bool hovered = (mouse.x >= bMin.x && mouse.x < bMax.x &&
                        mouse.y >= bMin.y && mouse.y < bMax.y);
        bool held = hovered && io.MouseDown[0];
        bool clicked = hovered && io.MouseReleased[0];
        titlebarWidgetHovered_ |= hovered;

        if (held)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(activeBg), 4.0f);
        else if (hovered)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(hoverBg), 4.0f);

        fg->AddText({bMin.x + 8.0f, (bMin.y + bMax.y - wsTextSize.y) * 0.5f},
                    ImGui::GetColorU32(colors.text), wsLabel.c_str());

        if (clicked) {
            openWorkspacePopup_ = true;
            workspacePopupPos_ = {wsX, origin.y + tbHeight};
        }
    }

    // Track right interactive zone start for hit testing
    interactiveRightStart_ = wsX;

    // ===================== LEFT SIDE: Sidebar toggle + Add connection =====================
    const float leftPad = 8.0f;
    float lx = origin.x + leftPad;

    // Sidebar toggle (hamburger icon drawn manually)
    {
        float iy = origin.y + (tbHeight - iconBtnSize) * 0.5f;
        ImVec2 bMin{lx, iy}, bMax{lx + iconBtnSize, iy + iconBtnSize};
        bool hovered = (mouse.x >= bMin.x && mouse.x < bMax.x &&
                        mouse.y >= bMin.y && mouse.y < bMax.y);
        bool held = hovered && io.MouseDown[0];
        bool clicked = hovered && io.MouseReleased[0];
        titlebarWidgetHovered_ |= hovered;
        if (held) fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(activeBg), 4.0f);
        else if (hovered) fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(hoverBg), 4.0f);

        ImVec2 center{(bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f};
        ImU32 iconCol = ImGui::GetColorU32(colors.text);
        fg->AddLine({center.x - 5, center.y - 4}, {center.x + 5, center.y - 4}, iconCol, 1.5f);
        fg->AddLine({center.x - 5, center.y},     {center.x + 5, center.y},     iconCol, 1.5f);
        fg->AddLine({center.x - 5, center.y + 4}, {center.x + 5, center.y + 4}, iconCol, 1.5f);

        if (clicked) onSidebarToggleClicked();
    }
    lx += iconBtnSize + 2.0f;

    // Add connection button (+)
    if (fgIconBtn(lx, ICON_FA_PLUS)) {
        if (app_->getDatabaseSidebar()) {
            app_->getDatabaseSidebar()->showConnectionDialog();
        }
    }
    lx += iconBtnSize + 8.0f;

    // Track left interactive zone end for hit testing
    interactiveLeftEnd_ = lx;

    // --- Title text ---
    const float titleY = origin.y + (tbHeight - ImGui::GetFontSize()) * 0.5f;
#ifdef NDEBUG
    fg->AddText({lx, titleY}, ImGui::GetColorU32(colors.subtext1), APP_NAME);
#else
    fg->AddText({lx, titleY}, ImGui::GetColorU32(colors.subtext1), "DearSQL (Debug)");
#endif
}

// ---------------------------------------------------------------------------
// Titlebar popups (workspace dropdown, menu)
// ---------------------------------------------------------------------------

void WindowsPlatform::renderTitlebarPopups() {
    if (!app_) return;

    const bool isDark = app_->isDarkTheme();
    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    // We need an ImGui window context to host popups
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({1, 1});
    ImGui::Begin("##TitlebarPopupHost", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);

    // --- Workspace popup ---
    if (openWorkspacePopup_) {
        ImGui::OpenPopup("##WorkspacePopup");
        openWorkspacePopup_ = false;
    }
    ImGui::SetNextWindowPos(workspacePopupPos_);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6, 6});
    if (ImGui::BeginPopup("##WorkspacePopup")) {
        auto workspaces = app_->getWorkspaces();
        for (const auto& ws : workspaces) {
            bool isCurrent = (ws.id == app_->getCurrentWorkspaceId());
            if (ImGui::Selectable(ws.name.c_str(), isCurrent)) {
                app_->setCurrentWorkspace(ws.id);
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("New Workspace...")) {
            app_->createWorkspace("New Workspace", "");
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // --- Menu popup ---
    if (openMenuPopup_) {
        ImGui::OpenPopup("##MenuPopup");
        openMenuPopup_ = false;
    }
    // Anchor popup so its RIGHT edge aligns with the menu button's right edge
    const float popupW = 220.0f;
    const float iconBtnSz = static_cast<float>(getTitlebarHeightPixels()) - 4.0f;
    ImGui::SetNextWindowPos({menuPopupPos_.x + iconBtnSz - popupW, menuPopupPos_.y});
    ImGui::SetNextWindowSize({popupW, 0});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8, 8});
    if (ImGui::BeginPopup("##MenuPopup")) {
        const float contentW = popupW - 24.0f; // minus padding

        // --- Theme section ---
        ImGui::TextColored(colors.subtext0, "Theme");
        {
            float btnW = (contentW - 8.0f) / 2.0f; // 2 buttons with 8px gap
            auto themeBtn = [&](const char* label, bool selected, auto action) {
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, colors.blue);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.blue);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                }
                if (ImGui::Button(label, {btnW, 0})) action();
                if (selected) ImGui::PopStyleColor(3);
            };
            themeBtn(ICON_FA_SUN "  Light", !isDark, [&] { app_->setDarkTheme(false); });
            ImGui::SameLine();
            themeBtn(ICON_FA_MOON "  Dark", isDark, [&] { app_->setDarkTheme(true); });
        }

        ImGui::Spacing();

        // --- Font size section ---
        ImGui::TextColored(colors.subtext0, "Font Size");
        {
            float sideBtnW = 36.0f;
            float currentScale = app_->getFontScale();
            if (ImGui::Button("A-", {sideBtnW, 0})) {
                app_->setFontScale(currentScale - 0.1f);
            }
            ImGui::SameLine();
            char sizeLabel[16];
            snprintf(sizeLabel, sizeof(sizeLabel), "%d%%", static_cast<int>(currentScale * 100));
            float centerW = contentW - sideBtnW * 2 - 8.0f * 2;
            float textW = ImGui::CalcTextSize(sizeLabel).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (centerW - textW) * 0.5f);
            ImGui::TextUnformatted(sizeLabel);
            ImGui::SameLine();
            float aplusX = ImGui::GetWindowContentRegionMax().x - sideBtnW;
            ImGui::SetCursorPosX(aplusX);
            if (ImGui::Button("A+", {sideBtnW, 0})) {
                app_->setFontScale(currentScale + 0.1f);
            }
        }

        ImGui::Separator();

        // --- Action buttons ---
        if (ImGui::Selectable("Manage License...")) {
            showLicenseDialog();
        }
        if (ImGui::Selectable("Report Bug...")) {
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/nicholasgasior/dearsql/issues/new"
                          L"?labels=bug&title=%5BBug%5D",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void WindowsPlatform::renderFrame() {
    if (!d3dDevice_ || !swapChain_ || !mainRenderTargetView_) {
        return;
    }

    if (app_ && app_->isDarkTheme() != lastAppliedDarkTheme_) {
        applyTitlebarTheme();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    renderTitlebar();
    renderTitlebarPopups();
    app_->renderMainUI();

    ImGui::Render();

    const auto& clearCol = app_->isDarkTheme() ? Theme::NATIVE_DARK.base : Theme::NATIVE_LIGHT.base;
    const float clearColor[4] = {clearCol.x, clearCol.y, clearCol.z, clearCol.w};
    d3dDeviceContext_->OMSetRenderTargets(1, &mainRenderTargetView_, nullptr);
    d3dDeviceContext_->ClearRenderTargetView(mainRenderTargetView_, clearColor);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swapChain_->Present(1, 0);
}

void WindowsPlatform::shutdownImGui() {
    ImGui_ImplDX11_Shutdown();
    std::cout << "ImGui DirectX 11 backend shutdown" << std::endl;
}

void WindowsPlatform::updateWorkspaceDropdown() {}

HWND WindowsPlatform::getHWND() const {
    if (!window_) {
        return nullptr;
    }
    return glfwGetWin32Window(window_);
}

void WindowsPlatform::createTitlebarControls(HWND) {}
void WindowsPlatform::destroyTitlebarControls() {}
void WindowsPlatform::layoutTitlebarControls() {}
void WindowsPlatform::showCreateWorkspaceDialog() {}

LRESULT WindowsPlatform::handleWindowMessage(HWND, UINT, WPARAM, LPARAM, bool& handled) {
    handled = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Native License Dialog
// ---------------------------------------------------------------------------

namespace {

// Control IDs for the license dialog
enum {
    IDC_LICENSE_KEY_EDIT = 200,
    IDC_LICENSE_STATUS_LABEL,
    IDC_LICENSE_ACTIVATE_BTN,
    IDC_LICENSE_DEACTIVATE_BTN,
    IDC_LICENSE_CLOSE_BTN,
    IDC_LICENSE_CANCEL_BTN,
    IDC_LICENSE_PURCHASE_LINK,
};

struct LicenseDialogData {
    WindowsPlatform* platform = nullptr;
    Application* app = nullptr;
    HWND dialog = nullptr;
    HWND statusLabel = nullptr;
    HWND actionButton = nullptr; // activate or deactivate
    bool licensed = false;
};

static HFONT sDialogFont = nullptr;
static HFONT sDialogBoldFont = nullptr;

HFONT getDialogFont() {
    if (!sDialogFont) {
        NONCLIENTMETRICSW ncm = {sizeof(ncm)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        sDialogFont = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return sDialogFont;
}

HFONT getDialogBoldFont() {
    if (!sDialogBoldFont) {
        NONCLIENTMETRICSW ncm = {sizeof(ncm)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfWeight = FW_BOLD;
        ncm.lfMessageFont.lfHeight = static_cast<LONG>(ncm.lfMessageFont.lfHeight * 1.2);
        sDialogBoldFont = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return sDialogBoldFont;
}

void setFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                 DWORD style = 0, int id = -1) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text,
                                WS_CHILD | WS_VISIBLE | SS_LEFT | style,
                                x, y, w, h, parent,
                                id >= 0 ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    setFont(hwnd, getDialogFont());
    return hwnd;
}

LRESULT CALLBACK LicenseDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<LicenseDialogData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        data = static_cast<LicenseDialogData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        data->dialog = hWnd;

        auto& lm = LicenseManager::instance();
        data->licensed = lm.hasValidLicense();

        constexpr int pad = 20;
        constexpr int labelH = 20;
        constexpr int editH = 24;
        constexpr int btnH = 28;
        constexpr int btnW = 100;
        int y = pad;
        int contentW = 410;

        if (data->licensed) {
            const auto info = lm.getLicenseInfo();

            // Title
            HWND title = createLabel(hWnd, L"License Active", pad, y, contentW, 28);
            setFont(title, getDialogBoldFont());
            y += 32;

            // Email
            createLabel(hWnd, L"Email:", pad, y, 80, labelH);
            std::wstring email = info.customerEmail.empty()
                ? L"N/A"
                : std::wstring(info.customerEmail.begin(), info.customerEmail.end());
            createLabel(hWnd, email.c_str(), pad + 85, y, contentW - 85, labelH);
            y += labelH + 6;

            // Key (masked)
            createLabel(hWnd, L"Key:", pad, y, 80, labelH);
            std::string maskedKey = info.licenseKey;
            if (maskedKey.length() > 8) {
                maskedKey = maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
            }
            std::wstring wKey(maskedKey.begin(), maskedKey.end());
            createLabel(hWnd, wKey.c_str(), pad + 85, y, contentW - 85, labelH);
            y += labelH + 6;

            // Device ID
            createLabel(hWnd, L"Device ID:", pad, y, 80, labelH);
            std::string deviceId = lm.getInstanceId();
            std::wstring wDeviceId(deviceId.begin(), deviceId.end());
            createLabel(hWnd, wDeviceId.c_str(), pad + 85, y, contentW - 85, labelH);
            y += labelH + 12;

            // Status label
            data->statusLabel = createLabel(hWnd, L"", pad, y, contentW, labelH,
                                            0, IDC_LICENSE_STATUS_LABEL);
            y += labelH + 12;

            // Deactivate button
            data->actionButton = CreateWindowExW(0, L"BUTTON", L"Deactivate",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                contentW + pad - btnW * 2 - 10, y, btnW, btnH, hWnd,
                reinterpret_cast<HMENU>(IDC_LICENSE_DEACTIVATE_BTN),
                GetModuleHandleW(nullptr), nullptr);
            setFont(data->actionButton, getDialogFont());

            // Close button
            HWND closeBtn = CreateWindowExW(0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                contentW + pad - btnW, y, btnW, btnH, hWnd,
                reinterpret_cast<HMENU>(IDC_LICENSE_CLOSE_BTN),
                GetModuleHandleW(nullptr), nullptr);
            setFont(closeBtn, getDialogFont());
            y += btnH + pad;

        } else {
            // Title
            HWND title = createLabel(hWnd, L"Register License", pad, y, contentW, 28);
            setFont(title, getDialogBoldFont());
            y += 32;

            // Description
            createLabel(hWnd, L"Enter your license key to activate DearSQL:", pad, y, contentW, labelH);
            y += labelH + 8;

            // Key input
            HWND keyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad, y, contentW, editH, hWnd,
                reinterpret_cast<HMENU>(IDC_LICENSE_KEY_EDIT),
                GetModuleHandleW(nullptr), nullptr);
            setFont(keyEdit, getDialogFont());
            SendMessageW(keyEdit, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"XXXX-XXXX-XXXX-XXXX"));
            y += editH + 6;

            // Device ID
            createLabel(hWnd, L"Device ID:", pad, y, 80, labelH);
            std::string deviceId = lm.getInstanceId();
            std::wstring wDeviceId(deviceId.begin(), deviceId.end());
            createLabel(hWnd, wDeviceId.c_str(), pad + 85, y, contentW - 85, labelH);
            y += labelH + 8;

            // Status label
            data->statusLabel = createLabel(hWnd, L"", pad, y, contentW, labelH,
                                            0, IDC_LICENSE_STATUS_LABEL);
            y += labelH + 8;

            // Purchase link
            createLabel(hWnd, L"Don't have a license?", pad, y, 150, labelH);
            HWND link = CreateWindowExW(0, L"SysLink",
                L"<a href=\"https://buy.polar.sh/polar_cl_IpYdAWiNljfzsXgatypm2mg40Mm2c4hB0DcVX1L9P6p\">Purchase one</a>",
                WS_CHILD | WS_VISIBLE,
                pad + 150, y, 150, labelH, hWnd,
                reinterpret_cast<HMENU>(IDC_LICENSE_PURCHASE_LINK),
                GetModuleHandleW(nullptr), nullptr);
            setFont(link, getDialogFont());
            y += labelH + 12;

            // Cancel button
            HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                contentW + pad - btnW * 2 - 10, y, btnW, btnH, hWnd,
                reinterpret_cast<HMENU>(IDC_LICENSE_CANCEL_BTN),
                GetModuleHandleW(nullptr), nullptr);
            setFont(cancelBtn, getDialogFont());

            // Activate button
            data->actionButton = CreateWindowExW(0, L"BUTTON", L"Activate",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                contentW + pad - btnW, y, btnW, btnH, hWnd,
                reinterpret_cast<HMENU>(IDC_LICENSE_ACTIVATE_BTN),
                GetModuleHandleW(nullptr), nullptr);
            setFont(data->actionButton, getDialogFont());
            y += btnH + pad;
        }

        // Resize window to fit content
        RECT rc = {0, 0, contentW + pad * 2, y};
        AdjustWindowRectEx(&rc, GetWindowLongW(hWnd, GWL_STYLE), FALSE,
                           GetWindowLongW(hWnd, GWL_EXSTYLE));
        SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        // Center on parent
        HWND parent = GetParent(hWnd);
        if (parent) {
            RECT parentRc;
            GetWindowRect(parent, &parentRc);
            RECT dlgRc;
            GetWindowRect(hWnd, &dlgRc);
            int cx = (parentRc.left + parentRc.right) / 2 - (dlgRc.right - dlgRc.left) / 2;
            int cy = (parentRc.top + parentRc.bottom) / 2 - (dlgRc.bottom - dlgRc.top) / 2;
            SetWindowPos(hWnd, nullptr, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }

        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_LICENSE_CLOSE_BTN || id == IDC_LICENSE_CANCEL_BTN) {
            DestroyWindow(hWnd);
            return 0;
        }

        if (id == IDC_LICENSE_ACTIVATE_BTN && data) {
            wchar_t keyBuf[256] = {};
            HWND keyEdit = GetDlgItem(hWnd, IDC_LICENSE_KEY_EDIT);
            GetWindowTextW(keyEdit, keyBuf, 256);
            std::wstring wKey(keyBuf);
            if (wKey.empty()) {
                SetWindowTextW(data->statusLabel, L"Please enter a license key");
                return 0;
            }

            SetWindowTextW(data->statusLabel, L"Activating...");
            EnableWindow(data->actionButton, FALSE);

            std::string key(wKey.begin(), wKey.end());
            HWND dlg = hWnd;

            LicenseManager::instance().activateLicense(key,
                [dlg](const LicenseInfo& result) {
                    bool valid = result.valid;
                    std::string err = result.error;
                    PostMessage(dlg, WM_APP + 1, valid ? 1 : 0, 0);
                    // Store error for retrieval — use window property
                    if (!valid) {
                        auto* errStr = new std::wstring(err.begin(), err.end());
                        if (errStr->empty()) *errStr = L"Activation failed";
                        SetPropW(dlg, L"LicenseError", errStr);
                    }
                });
            return 0;
        }

        if (id == IDC_LICENSE_DEACTIVATE_BTN && data) {
            SetWindowTextW(data->statusLabel, L"Deactivating...");
            EnableWindow(data->actionButton, FALSE);

            HWND dlg = hWnd;
            LicenseManager::instance().deactivateLicense(
                [dlg](const LicenseInfo& result) {
                    std::string err = result.error;
                    PostMessage(dlg, WM_APP + 2, err.empty() ? 1 : 0, 0);
                    if (!err.empty()) {
                        auto* errStr = new std::wstring(err.begin(), err.end());
                        SetPropW(dlg, L"LicenseError", errStr);
                    }
                });
            return 0;
        }
        break;
    }

    case WM_APP + 1: { // Activation result
        if (!data) break;
        if (wParam == 1) {
            DestroyWindow(hWnd);
        } else {
            auto* errStr = static_cast<std::wstring*>(
                RemovePropW(hWnd, L"LicenseError"));
            if (errStr) {
                SetWindowTextW(data->statusLabel, errStr->c_str());
                delete errStr;
            }
            EnableWindow(data->actionButton, TRUE);
        }
        return 0;
    }

    case WM_APP + 2: { // Deactivation result
        if (!data) break;
        if (wParam == 1) {
            DestroyWindow(hWnd);
        } else {
            auto* errStr = static_cast<std::wstring*>(
                RemovePropW(hWnd, L"LicenseError"));
            if (errStr) {
                SetWindowTextW(data->statusLabel, errStr->c_str());
                delete errStr;
            }
            EnableWindow(data->actionButton, TRUE);
        }
        return 0;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
        if (nmhdr->idFrom == IDC_LICENSE_PURCHASE_LINK && nmhdr->code == NM_CLICK) {
            ShellExecuteW(nullptr, L"open",
                L"https://buy.polar.sh/polar_cl_IpYdAWiNljfzsXgatypm2mg40Mm2c4hB0DcVX1L9P6p",
                nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }

    case WM_DESTROY: {
        // Clean up any leftover error string
        auto* errStr = static_cast<std::wstring*>(RemovePropW(hWnd, L"LicenseError"));
        delete errStr;
        auto* d = reinterpret_cast<LicenseDialogData*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        delete d;
        return 0;
    }
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

void WindowsPlatform::showLicenseDialog() {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = LicenseDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"DearSQL_LicenseDialog";
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    auto* data = new LicenseDialogData();
    data->platform = this;
    data->app = app_;

    HWND parent = getHWND();
    CreateWindowExW(WS_EX_DLGMODALFRAME, L"DearSQL_LicenseDialog",
                    L"Manage License",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, 450, 300,
                    parent, nullptr, GetModuleHandleW(nullptr), data);
}

// ---------------------------------------------------------------------------
// D3D11 device
// ---------------------------------------------------------------------------

bool WindowsPlatform::createD3DDevice(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    constexpr D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2,
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
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (d3dDeviceContext_) { d3dDeviceContext_->Release(); d3dDeviceContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
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

// ---------------------------------------------------------------------------
// Titlebar theme (DWM dark mode attribute)
// ---------------------------------------------------------------------------

void WindowsPlatform::applyTitlebarTheme() {
    HWND hWnd = getHWND();
    if (!hWnd || !app_) {
        return;
    }

    const bool isDark = app_->isDarkTheme();
    lastAppliedDarkTheme_ = isDark;

    const BOOL useDarkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
}

// ---------------------------------------------------------------------------
// Texture creation (D3D11)
// ---------------------------------------------------------------------------

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

    return (ImTextureID)(INT_PTR)srv;
}

#endif
