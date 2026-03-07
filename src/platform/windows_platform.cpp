#if defined(_WIN32)

#include "platform/windows_platform.hpp"
#include "application.hpp"
#include "imgui_impl_dx11.h"
#include "imgui_impl_glfw.h"
#include "platform/connection_dialog.hpp"
#include "themes.hpp"

#include <algorithm>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <iostream>
#include <string>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

namespace {

    constexpr wchar_t kPlatformPropName[] = L"DearSQL_WindowsPlatform";
    constexpr wchar_t kOldWndProcPropName[] = L"DearSQL_WindowsPlatformOldWndProc";
    constexpr wchar_t kCreateWorkspaceDialogClass[] = L"DearSQL_CreateWorkspaceDialog";

    enum : int {
        IDC_TITLEBAR_WORKSPACE = 5001,
        IDC_TITLEBAR_ADD_BUTTON,
        IDC_TITLEBAR_SIDEBAR_BUTTON,
        IDC_WORKSPACE_NAME_EDIT,
        IDC_WORKSPACE_CREATE_BUTTON,
        IDC_WORKSPACE_CANCEL_BUTTON,
    };

    HWND sActiveCreateWorkspaceDialog = nullptr;

    struct CreateWorkspaceDialogData {
        Application* app = nullptr;
        WindowsPlatform* platform = nullptr;
        HWND parentHwnd = nullptr;
        HWND nameEdit = nullptr;
    };

    std::wstring widen(const std::string& value) {
        if (value.empty()) {
            return {};
        }

        const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                               static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) {
            return {};
        }

        std::wstring wide(needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(),
                            needed);
        return wide;
    }

    std::string narrow(const std::wstring& value) {
        if (value.empty()) {
            return {};
        }

        const int needed =
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr,
                                0, nullptr, nullptr);
        if (needed <= 0) {
            return {};
        }

        std::string narrowValue(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                            narrowValue.data(), needed, nullptr, nullptr);
        return narrowValue;
    }

    std::string trim(std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }

        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    int getWindowDpi(HWND hWnd) {
        if (!hWnd) {
            return USER_DEFAULT_SCREEN_DPI;
        }
        return static_cast<int>(GetDpiForWindow(hWnd));
    }

    int scaleForWindow(HWND hWnd, int value) {
        return MulDiv(value, getWindowDpi(hWnd), USER_DEFAULT_SCREEN_DPI);
    }

    void applyDefaultGuiFont(HWND hWnd) {
        if (!hWnd) {
            return;
        }

        SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     TRUE);
    }

    bool pointInControl(HWND hWnd, POINT screenPt) {
        if (!hWnd || !IsWindowVisible(hWnd)) {
            return false;
        }

        RECT rect{};
        GetWindowRect(hWnd, &rect);
        return PtInRect(&rect, screenPt) != FALSE;
    }

    void ensureWindowClassRegistered(const wchar_t* className, WNDPROC proc) {
        WNDCLASSEXW wc{};
        if (GetClassInfoExW(GetModuleHandleW(nullptr), className, &wc)) {
            return;
        }

        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
    }

    void centerWindowToParent(HWND hWnd, HWND parentHwnd) {
        RECT parentRect{};
        if (parentHwnd && GetWindowRect(parentHwnd, &parentRect)) {
            // parent rect set
        } else {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &parentRect, 0);
        }

        RECT windowRect{};
        GetWindowRect(hWnd, &windowRect);

        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x =
            parentRect.left + std::max(0, ((parentRect.right - parentRect.left) - width) / 2);
        const int y =
            parentRect.top + std::max(0, ((parentRect.bottom - parentRect.top) - height) / 2);

        SetWindowPos(hWnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    CreateWorkspaceDialogData* getWorkspaceDialogData(HWND hWnd) {
        return reinterpret_cast<CreateWorkspaceDialogData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    LRESULT CALLBACK CreateWorkspaceDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* data = static_cast<CreateWorkspaceDialogData*>(create->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));

            const int padX = scaleForWindow(hWnd, 16);
            const int padY = scaleForWindow(hWnd, 16);
            const int labelY = padY;
            const int editY = labelY + scaleForWindow(hWnd, 22);
            const int editH = scaleForWindow(hWnd, 28);
            const int buttonY = editY + editH + scaleForWindow(hWnd, 16);
            const int buttonW = scaleForWindow(hWnd, 88);
            const int buttonH = scaleForWindow(hWnd, 28);
            const int gap = scaleForWindow(hWnd, 8);
            const int clientW = scaleForWindow(hWnd, 360);

            HWND label = CreateWindowExW(0, L"STATIC", L"Enter a name for the new workspace:",
                                         WS_CHILD | WS_VISIBLE, padX, labelY, clientW - padX * 2,
                                         scaleForWindow(hWnd, 18), hWnd, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
            applyDefaultGuiFont(label);

            data->nameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                             padX, editY, clientW - padX * 2, editH, hWnd,
                                             reinterpret_cast<HMENU>(IDC_WORKSPACE_NAME_EDIT),
                                             GetModuleHandleW(nullptr), nullptr);
            applyDefaultGuiFont(data->nameEdit);
            SendMessageW(data->nameEdit, EM_SETLIMITTEXT, 128, 0);

            HWND cancelButton = CreateWindowExW(
                0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                clientW - padX - buttonW * 2 - gap, buttonY, buttonW, buttonH, hWnd,
                reinterpret_cast<HMENU>(IDC_WORKSPACE_CANCEL_BUTTON), GetModuleHandleW(nullptr),
                nullptr);
            applyDefaultGuiFont(cancelButton);

            HWND createButton = CreateWindowExW(
                0, L"BUTTON", L"Create", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                clientW - padX - buttonW, buttonY, buttonW, buttonH, hWnd,
                reinterpret_cast<HMENU>(IDC_WORKSPACE_CREATE_BUTTON), GetModuleHandleW(nullptr),
                nullptr);
            applyDefaultGuiFont(createButton);

            centerWindowToParent(hWnd, data->parentHwnd);
            SetFocus(data->nameEdit);
            return 0;
        }

        case WM_COMMAND: {
            auto* data = getWorkspaceDialogData(hWnd);
            if (!data) {
                return 0;
            }

            const int controlId = LOWORD(wParam);
            if (controlId == IDC_WORKSPACE_CANCEL_BUTTON) {
                DestroyWindow(hWnd);
                return 0;
            }

            if (controlId != IDC_WORKSPACE_CREATE_BUTTON) {
                break;
            }

            const int textLen = GetWindowTextLengthW(data->nameEdit);
            std::wstring workspaceNameWide(static_cast<size_t>(textLen) + 1, L'\0');
            if (textLen > 0) {
                GetWindowTextW(data->nameEdit, workspaceNameWide.data(), textLen + 1);
            }
            workspaceNameWide.resize(static_cast<size_t>(textLen));

            const std::string workspaceName = trim(narrow(workspaceNameWide));
            if (workspaceName.empty()) {
                MessageBoxW(hWnd, L"Workspace name cannot be empty.", L"Create Workspace",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }

            if (!data->app) {
                DestroyWindow(hWnd);
                return 0;
            }

            const int newWorkspaceId = data->app->createWorkspace(workspaceName);
            if (newWorkspaceId <= 0) {
                MessageBoxW(hWnd, L"Failed to create workspace.", L"Create Workspace",
                            MB_OK | MB_ICONERROR);
                return 0;
            }

            DestroyWindow(hWnd);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY: {
            auto* data = getWorkspaceDialogData(hWnd);
            sActiveCreateWorkspaceDialog = nullptr;
            delete data;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            return 0;
        }

        default:
            break;
        }

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

} // namespace

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
    HWND hWnd = getHWND();
    if (!hWnd) {
        return;
    }

    titlebarHeightPx_ = scaleForWindow(hWnd, 40);
    if (!oldWndProc_) {
        SetPropW(hWnd, kPlatformPropName, this);
        oldWndProc_ = reinterpret_cast<void*>(SetWindowLongPtrW(
            hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowsPlatform::TitlebarWindowProc)));
        SetPropW(hWnd, kOldWndProcPropName, oldWndProc_);
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    createTitlebarControls(hWnd);
    layoutTitlebarControls();
    updateWorkspaceDropdown();
    applyTitlebarTheme();
    std::cout << "Windows titlebar configured" << std::endl;
}

float WindowsPlatform::getTitlebarHeight() const {
    return static_cast<float>(titlebarHeightPx_);
}

float WindowsPlatform::getClientAreaTopInset() const {
    return static_cast<float>(titlebarHeightPx_);
}

void WindowsPlatform::onSidebarToggleClicked() {
    if (app_) {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    }
}

void WindowsPlatform::cleanup() {
    destroyTitlebarControls();

    if (HWND hWnd = getHWND(); hWnd && oldWndProc_) {
        SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oldWndProc_));
        RemovePropW(hWnd, kPlatformPropName);
        RemovePropW(hWnd, kOldWndProcPropName);
        oldWndProc_ = nullptr;
    }

    cleanupD3DDevice();
}

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
    if (!workspaceDropdown_ || !app_) {
        return;
    }

    updatingWorkspaceDropdown_ = true;
    SendMessageW(workspaceDropdown_, CB_RESETCONTENT, 0, 0);
    workspaceIdsByIndex_.clear();

    const auto workspaces = app_->getWorkspaces();
    const int currentWorkspaceId = app_->getCurrentWorkspaceId();
    int selectedIndex = 0;

    for (std::size_t i = 0; i < workspaces.size(); ++i) {
        const auto wideName = widen(workspaces[i].name);
        const LRESULT index = SendMessageW(workspaceDropdown_, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(wideName.c_str()));
        if (index >= 0) {
            workspaceIdsByIndex_.push_back(workspaces[i].id);
            if (workspaces[i].id == currentWorkspaceId) {
                selectedIndex = static_cast<int>(index);
            }
        }
    }

    newWorkspaceItemIndex_ = static_cast<int>(SendMessageW(
        workspaceDropdown_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"New Workspace...")));
    SendMessageW(workspaceDropdown_, CB_SETCURSEL, selectedIndex, 0);
    updatingWorkspaceDropdown_ = false;
}

HWND WindowsPlatform::getHWND() const {
    if (!window_) {
        return nullptr;
    }
    return glfwGetWin32Window(window_);
}

void WindowsPlatform::createTitlebarControls(HWND hWnd) {
    if (!sidebarButton_) {
        sidebarButton_ = CreateWindowExW(
            0, L"BUTTON", L"Sidebar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0,
            0, hWnd, reinterpret_cast<HMENU>(IDC_TITLEBAR_SIDEBAR_BUTTON),
            GetModuleHandleW(nullptr), nullptr);
        applyDefaultGuiFont(sidebarButton_);
    }

    if (!addButton_) {
        addButton_ = CreateWindowExW(0, L"BUTTON", L"Add",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                                     hWnd, reinterpret_cast<HMENU>(IDC_TITLEBAR_ADD_BUTTON),
                                     GetModuleHandleW(nullptr), nullptr);
        applyDefaultGuiFont(addButton_);
    }

    if (!workspaceDropdown_) {
        workspaceDropdown_ = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, 0,
            0, 0, 240, hWnd, reinterpret_cast<HMENU>(IDC_TITLEBAR_WORKSPACE),
            GetModuleHandleW(nullptr), nullptr);
        applyDefaultGuiFont(workspaceDropdown_);
    }
}

void WindowsPlatform::destroyTitlebarControls() {
    if (workspaceDropdown_) {
        DestroyWindow(workspaceDropdown_);
        workspaceDropdown_ = nullptr;
    }

    if (addButton_) {
        DestroyWindow(addButton_);
        addButton_ = nullptr;
    }

    if (sidebarButton_) {
        DestroyWindow(sidebarButton_);
        sidebarButton_ = nullptr;
    }

    workspaceIdsByIndex_.clear();
    newWorkspaceItemIndex_ = -1;
}

void WindowsPlatform::layoutTitlebarControls() {
    HWND hWnd = getHWND();
    if (!hWnd) {
        return;
    }

    RECT clientRect{};
    GetClientRect(hWnd, &clientRect);

    const int dpi = getWindowDpi(hWnd);
    titlebarHeightPx_ = MulDiv(40, dpi, USER_DEFAULT_SCREEN_DPI);

    const int pad = MulDiv(8, dpi, USER_DEFAULT_SCREEN_DPI);
    const int buttonH = titlebarHeightPx_ - pad * 2;
    const int sidebarW = MulDiv(72, dpi, USER_DEFAULT_SCREEN_DPI);
    const int addW = MulDiv(52, dpi, USER_DEFAULT_SCREEN_DPI);
    const int comboMinW = MulDiv(150, dpi, USER_DEFAULT_SCREEN_DPI);
    const int comboMaxW = MulDiv(260, dpi, USER_DEFAULT_SCREEN_DPI);
    const int captionButtonsW = MulDiv(3 * GetSystemMetricsForDpi(SM_CXSIZE, dpi), 1, 1);

    int x = pad;
    const int y = pad;

    if (sidebarButton_) {
        MoveWindow(sidebarButton_, x, y, sidebarW, buttonH, TRUE);
        x += sidebarW + pad;
    }

    if (addButton_) {
        MoveWindow(addButton_, x, y, addW, buttonH, TRUE);
    }

    if (workspaceDropdown_) {
        const int availableRight = std::max(pad, clientRect.right - captionButtonsW - pad);
        const int comboW = std::clamp(MulDiv(clientRect.right, 22, 100), comboMinW, comboMaxW);
        const int comboX = std::max(x + addW + pad, availableRight - comboW);
        MoveWindow(workspaceDropdown_, comboX, y, comboW, scaleForWindow(hWnd, 400), TRUE);
    }
}

void WindowsPlatform::showCreateWorkspaceDialog() {
    if (sActiveCreateWorkspaceDialog) {
        SetForegroundWindow(sActiveCreateWorkspaceDialog);
        return;
    }

    ensureWindowClassRegistered(kCreateWorkspaceDialogClass, CreateWorkspaceDialogProc);

    auto* data = new CreateWorkspaceDialogData();
    data->app = app_;
    data->platform = this;
    data->parentHwnd = getHWND();

    HWND hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kCreateWorkspaceDialogClass, L"Create New Workspace",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        scaleForWindow(getHWND(), 360), scaleForWindow(getHWND(), 170), data->parentHwnd, nullptr,
        GetModuleHandleW(nullptr), data);
    sActiveCreateWorkspaceDialog = hWnd;
}

LRESULT WindowsPlatform::handleWindowMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             bool& handled) {
    handled = true;

    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(hWnd)) {
                MONITORINFO monitorInfo{sizeof(monitorInfo)};
                if (GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST),
                                    &monitorInfo)) {
                    params->rgrc[0] = monitorInfo.rcWork;
                }
            }
            return 0;
        }
        handled = false;
        return 0;

    case WM_NCHITTEST: {
        LRESULT hitResult = 0;
        if (DwmDefWindowProc(hWnd, msg, wParam, lParam, &hitResult)) {
            return hitResult;
        }

        const POINT screenPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (pointInControl(sidebarButton_, screenPt) || pointInControl(addButton_, screenPt) ||
            pointInControl(workspaceDropdown_, screenPt)) {
            return HTCLIENT;
        }

        RECT windowRect{};
        GetWindowRect(hWnd, &windowRect);

        if (!IsZoomed(hWnd)) {
            const int dpi = getWindowDpi(hWnd);
            const int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

            const bool left = screenPt.x < windowRect.left + frameX;
            const bool right = screenPt.x >= windowRect.right - frameX;
            const bool top = screenPt.y < windowRect.top + frameY;
            const bool bottom = screenPt.y >= windowRect.bottom - frameY;

            if (top && left) {
                return HTTOPLEFT;
            }
            if (top && right) {
                return HTTOPRIGHT;
            }
            if (bottom && left) {
                return HTBOTTOMLEFT;
            }
            if (bottom && right) {
                return HTBOTTOMRIGHT;
            }
            if (top) {
                return HTTOP;
            }
            if (left) {
                return HTLEFT;
            }
            if (right) {
                return HTRIGHT;
            }
            if (bottom) {
                return HTBOTTOM;
            }
        }

        if (screenPt.y < windowRect.top + titlebarHeightPx_) {
            return HTCAPTION;
        }

        handled = false;
        return 0;
    }

    case WM_COMMAND: {
        const int controlId = LOWORD(wParam);
        const int notification = HIWORD(wParam);

        if (controlId == IDC_TITLEBAR_SIDEBAR_BUTTON && notification == BN_CLICKED) {
            onSidebarToggleClicked();
            return 0;
        }

        if (controlId == IDC_TITLEBAR_ADD_BUTTON && notification == BN_CLICKED) {
            if (app_) {
                showConnectionDialog(app_);
            }
            return 0;
        }

        if (controlId == IDC_TITLEBAR_WORKSPACE && notification == CBN_SELCHANGE &&
            !updatingWorkspaceDropdown_ && workspaceDropdown_ && app_) {
            const int selectedIndex =
                static_cast<int>(SendMessageW(workspaceDropdown_, CB_GETCURSEL, 0, 0));
            if (selectedIndex == CB_ERR) {
                return 0;
            }

            if (selectedIndex == newWorkspaceItemIndex_) {
                updateWorkspaceDropdown();
                showCreateWorkspaceDialog();
                return 0;
            }

            if (selectedIndex >= 0 &&
                selectedIndex < static_cast<int>(workspaceIdsByIndex_.size())) {
                app_->setCurrentWorkspace(workspaceIdsByIndex_[selectedIndex]);
                updateWorkspaceDropdown();
            }
            return 0;
        }

        handled = false;
        return 0;
    }

    case WM_SIZE:
        layoutTitlebarControls();
        handled = false;
        return 0;

    case WM_DPICHANGED: {
        if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
            SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        titlebarHeightPx_ = scaleForWindow(hWnd, 40);
        layoutTitlebarControls();
        applyTitlebarTheme();
        return 0;
    }

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_ACTIVATE:
        applyTitlebarTheme();
        handled = false;
        return 0;

    case WM_DESTROY:
        destroyTitlebarControls();
        handled = false;
        return 0;

    default:
        handled = false;
        return 0;
    }
}

LRESULT CALLBACK WindowsPlatform::TitlebarWindowProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                     LPARAM lParam) {
    auto* platform = reinterpret_cast<WindowsPlatform*>(GetPropW(hWnd, kPlatformPropName));
    auto* oldWndProc = reinterpret_cast<WNDPROC>(GetPropW(hWnd, kOldWndProcPropName));

    if (!platform || !oldWndProc) {
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    bool handled = false;
    const LRESULT result = platform->handleWindowMessage(hWnd, msg, wParam, lParam, handled);
    if (handled) {
        return result;
    }

    return CallWindowProcW(oldWndProc, hWnd, msg, wParam, lParam);
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

    const bool isDark = app_->isDarkTheme();
    lastAppliedDarkTheme_ = isDark;

    const BOOL useDarkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    const COLORREF captionColor =
        RGB(static_cast<int>(colors.mantle.x * 255), static_cast<int>(colors.mantle.y * 255),
            static_cast<int>(colors.mantle.z * 255));
    DwmSetWindowAttribute(hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    const MARGINS margins = {0, 0, titlebarHeightPx_, 0};
    DwmExtendFrameIntoClientArea(hWnd, &margins);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
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

    return (ImTextureID)(INT_PTR)srv;
}

#endif
