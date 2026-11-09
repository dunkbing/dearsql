#if defined(_WIN32)

#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "mydear/title_bar.hpp"
#include "platform/alert.hpp"
#include "platform/titlebar.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/title_bar.hpp"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <iostream>
#include <shellapi.h>
#include <string>
#include <windowsx.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

WindowsTitlebar::WindowsTitlebar(Application* app, GLFWwindow* window)
    : app_(app), window_(window) {}

void WindowsTitlebar::setup() {
    // native-only actions the shared settings dialog can't do itself
    SettingsDialog::instance().onManageLicense = [this]() { showLicenseDialog(); };
    SettingsDialog::instance().onReportBug = []() {
        ShellExecuteW(nullptr, L"open",
                      L"https://github.com/dunkbing/dearsql/issues/new?labels=bug&title=%5BBug%5D",
                      nullptr, nullptr, SW_SHOWNORMAL);
    };
}

void WindowsTitlebar::render() {
    TitleBarView::render(*app_);
}

float WindowsTitlebar::getHeight() const {
    return mydear::TitleBarHeight();
}

// ---------------------------------------------------------------------------
// Native License Dialog
// ---------------------------------------------------------------------------

namespace {

    // control IDs for the license dialog
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

    HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, DWORD style = 0,
                     int id = -1) {
        HWND hwnd = CreateWindowExW(
            0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | style, x, y, w, h, parent,
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

                // title
                HWND title = createLabel(hWnd, L"License Active", pad, y, contentW, 28);
                setFont(title, getDialogBoldFont());
                y += 32;

                // email
                createLabel(hWnd, L"Email:", pad, y, 80, labelH);
                std::wstring email =
                    info.customerEmail.empty()
                        ? L"N/A"
                        : std::wstring(info.customerEmail.begin(), info.customerEmail.end());
                createLabel(hWnd, email.c_str(), pad + 85, y, contentW - 85, labelH);
                y += labelH + 6;

                // key (masked)
                createLabel(hWnd, L"Key:", pad, y, 80, labelH);
                std::string maskedKey = info.licenseKey;
                if (maskedKey.length() > 8) {
                    maskedKey =
                        maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
                }
                std::wstring wKey(maskedKey.begin(), maskedKey.end());
                createLabel(hWnd, wKey.c_str(), pad + 85, y, contentW - 85, labelH);
                y += labelH + 6;

                // device ID
                createLabel(hWnd, L"Device ID:", pad, y, 80, labelH);
                std::string deviceId = lm.getInstanceId();
                std::wstring wDeviceId(deviceId.begin(), deviceId.end());
                createLabel(hWnd, wDeviceId.c_str(), pad + 85, y, contentW - 85, labelH);
                y += labelH + 12;

                // status label
                data->statusLabel =
                    createLabel(hWnd, L"", pad, y, contentW, labelH, 0, IDC_LICENSE_STATUS_LABEL);
                y += labelH + 12;

                // deactivate button
                data->actionButton = CreateWindowExW(
                    0, L"BUTTON", L"Deactivate", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    contentW + pad - btnW * 2 - 10, y, btnW, btnH, hWnd,
                    reinterpret_cast<HMENU>(IDC_LICENSE_DEACTIVATE_BTN), GetModuleHandleW(nullptr),
                    nullptr);
                setFont(data->actionButton, getDialogFont());

                // close button
                HWND closeBtn =
                    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    contentW + pad - btnW, y, btnW, btnH, hWnd,
                                    reinterpret_cast<HMENU>(IDC_LICENSE_CLOSE_BTN),
                                    GetModuleHandleW(nullptr), nullptr);
                setFont(closeBtn, getDialogFont());
                y += btnH + pad;

            } else {
                // title
                HWND title = createLabel(hWnd, L"Register License", pad, y, contentW, 28);
                setFont(title, getDialogBoldFont());
                y += 32;

                // description
                createLabel(hWnd, L"Enter your license key to activate DearSQL:", pad, y, contentW,
                            labelH);
                y += labelH + 8;

                // key input
                HWND keyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                               pad, y, contentW, editH, hWnd,
                                               reinterpret_cast<HMENU>(IDC_LICENSE_KEY_EDIT),
                                               GetModuleHandleW(nullptr), nullptr);
                setFont(keyEdit, getDialogFont());
                SendMessageW(keyEdit, EM_SETCUEBANNER, 0,
                             reinterpret_cast<LPARAM>(L"XXXX-XXXX-XXXX-XXXX"));
                y += editH + 6;

                // device ID
                createLabel(hWnd, L"Device ID:", pad, y, 80, labelH);
                std::string deviceId = lm.getInstanceId();
                std::wstring wDeviceId(deviceId.begin(), deviceId.end());
                createLabel(hWnd, wDeviceId.c_str(), pad + 85, y, contentW - 85, labelH);
                y += labelH + 8;

                // status label
                data->statusLabel =
                    createLabel(hWnd, L"", pad, y, contentW, labelH, 0, IDC_LICENSE_STATUS_LABEL);
                y += labelH + 8;

                // purchase link
                createLabel(hWnd, L"Don't have a license?", pad, y, 150, labelH);
                HWND link = CreateWindowExW(0, L"SysLink",
                                            L"<a href=\"" PURCHASE_URL L"\">Purchase one</a>",
                                            WS_CHILD | WS_VISIBLE, pad + 150, y, 150, labelH, hWnd,
                                            reinterpret_cast<HMENU>(IDC_LICENSE_PURCHASE_LINK),
                                            GetModuleHandleW(nullptr), nullptr);
                setFont(link, getDialogFont());
                y += labelH + 12;

                // cancel button
                HWND cancelBtn =
                    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    contentW + pad - btnW * 2 - 10, y, btnW, btnH, hWnd,
                                    reinterpret_cast<HMENU>(IDC_LICENSE_CANCEL_BTN),
                                    GetModuleHandleW(nullptr), nullptr);
                setFont(cancelBtn, getDialogFont());

                // activate button
                data->actionButton = CreateWindowExW(
                    0, L"BUTTON", L"Activate", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    contentW + pad - btnW, y, btnW, btnH, hWnd,
                    reinterpret_cast<HMENU>(IDC_LICENSE_ACTIVATE_BTN), GetModuleHandleW(nullptr),
                    nullptr);
                setFont(data->actionButton, getDialogFont());
                y += btnH + pad;
            }

            // resize window to fit content
            RECT rc = {0, 0, contentW + pad * 2, y};
            AdjustWindowRectEx(&rc, GetWindowLongW(hWnd, GWL_STYLE), FALSE,
                               GetWindowLongW(hWnd, GWL_EXSTYLE));
            SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOMOVE | SWP_NOZORDER);

            // center on parent
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

                LicenseManager::instance().activateLicense(key, [dlg](const LicenseInfo& result) {
                    bool valid = result.valid;
                    std::string err = result.error;
                    PostMessage(dlg, WM_APP + 1, valid ? 1 : 0, 0);
                    // store error for retrieval — use window property
                    if (!valid) {
                        auto* errStr = new std::wstring(err.begin(), err.end());
                        if (errStr->empty())
                            *errStr = L"Activation failed";
                        SetPropW(dlg, L"LicenseError", errStr);
                    }
                });
                return 0;
            }

            if (id == IDC_LICENSE_DEACTIVATE_BTN && data) {
                SetWindowTextW(data->statusLabel, L"Deactivating...");
                EnableWindow(data->actionButton, FALSE);

                HWND dlg = hWnd;
                LicenseManager::instance().deactivateLicense([dlg](const LicenseInfo& result) {
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

        case WM_APP + 1: { // activation result
            if (!data)
                break;
            if (wParam == 1) {
                DestroyWindow(hWnd);
            } else {
                auto* errStr = static_cast<std::wstring*>(RemovePropW(hWnd, L"LicenseError"));
                if (errStr) {
                    SetWindowTextW(data->statusLabel, errStr->c_str());
                    delete errStr;
                }
                EnableWindow(data->actionButton, TRUE);
            }
            return 0;
        }

        case WM_APP + 2: { // deactivation result
            if (!data)
                break;
            if (wParam == 1) {
                DestroyWindow(hWnd);
            } else {
                auto* errStr = static_cast<std::wstring*>(RemovePropW(hWnd, L"LicenseError"));
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
                // ponytail: L"" widens the narrow PURCHASE_URL macro for ShellExecuteW
                ShellExecuteW(nullptr, L"open", L"" PURCHASE_URL, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            break;
        }

        case WM_DESTROY: {
            // clean up any leftover error string
            auto* errStr = static_cast<std::wstring*>(RemovePropW(hWnd, L"LicenseError"));
            delete errStr;
            auto* d = reinterpret_cast<LicenseDialogData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            delete d;
            return 0;
        }
        }

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

} // namespace

void WindowsTitlebar::showLicenseDialog() {
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
    data->app = app_;

    HWND parent = glfwGetWin32Window(window_);
    CreateWindowExW(WS_EX_DLGMODALFRAME, L"DearSQL_LicenseDialog", L"Manage License",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                    CW_USEDEFAULT, 450, 300, parent, nullptr, GetModuleHandleW(nullptr), data);
}

#endif
