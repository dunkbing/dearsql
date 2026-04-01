#if defined(_WIN32)

#include "application.hpp"
#include "platform/windows_platform.hpp"
#include "ui/input_dialog.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

static constexpr int ID_EDIT = 101;
static constexpr int ID_CONFIRM = 102;
static constexpr int ID_CANCEL = 103;
static constexpr int ID_ERROR = 104;
static constexpr int ID_LABEL = 105;
static constexpr int DIALOG_WIDTH = 420;

static auto toWide(const std::string& s) -> std::wstring {
    if (s.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wide.data(), len);
    return wide;
}

static auto fromWide(const wchar_t* s, int len) -> std::string {
    if (!s || len == 0)
        return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string str(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, str.data(), bytes, nullptr, nullptr);
    return str;
}

struct InputDialogState {
    InputDialog::ValidatorCallback validator;
    InputDialog::ConfirmCallback onConfirm;
    InputDialog::CancelCallback onCancel;
    HWND editControl = nullptr;
    HWND errorLabel = nullptr;
};

static LRESULT CALLBACK inputDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<InputDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND: {
        if (!state)
            break;
        int id = LOWORD(wParam);

        if (id == ID_CONFIRM) {
            int len = GetWindowTextLengthW(state->editControl);
            std::wstring wide(len + 1, L'\0');
            GetWindowTextW(state->editControl, wide.data(), len + 1);
            std::string value = fromWide(wide.c_str(), len);

            if (state->validator) {
                std::string error = state->validator(value);
                if (!error.empty()) {
                    SetWindowTextW(state->errorLabel, toWide(error).c_str());
                    ShowWindow(state->errorLabel, SW_SHOW);
                    return 0;
                }
            }

            if (state->onConfirm) {
                std::string error = state->onConfirm(value);
                if (!error.empty()) {
                    SetWindowTextW(state->errorLabel, toWide(error).c_str());
                    ShowWindow(state->errorLabel, SW_SHOW);
                    return 0;
                }
            }

            DestroyWindow(hwnd);
        } else if (id == ID_CANCEL) {
            if (state->onCancel)
                state->onCancel();
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        if (state && state->onCancel)
            state->onCancel();
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void registerInputDialogClass() {
    static bool registered = false;
    if (registered)
        return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = inputDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DearSQLInputDialog";
    RegisterClassExW(&wc);
    registered = true;
}

static void runModalLoop(HWND hwnd) {
    MSG msg{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        if (IsDialogMessageW(hwnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void InputDialog::show(const std::string& title, const std::string& label,
                       const std::string& initialValue, const std::string& confirmButtonText,
                       ConfirmCallback onConfirm, CancelCallback onCancel,
                       ValidatorCallback validator) {
    registerInputDialogClass();

    HWND parent = nullptr;
    auto* platform = dynamic_cast<WindowsPlatform*>(Application::getInstance().getPlatform());
    if (platform) {
        parent = platform->getHWND();
    }

    // calculate layout
    bool hasLabel = !label.empty();
    int y = 16;
    int labelY = y;
    if (hasLabel)
        y += 24;
    int editY = y;
    y += 32;
    int errorY = y;
    y += 40;
    int buttonY = y;
    int dialogHeight =
        buttonY + 28 + 16 + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME) * 2;

    // center on parent or screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - DIALOG_WIDTH) / 2;
    int posY = (screenH - dialogHeight) / 2;
    if (parent) {
        RECT pr;
        GetWindowRect(parent, &pr);
        posX = pr.left + ((pr.right - pr.left) - DIALOG_WIDTH) / 2;
        posY = pr.top + ((pr.bottom - pr.top) - dialogHeight) / 2;
    }

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DearSQLInputDialog", toWide(title).c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, posX, posY, DIALOG_WIDTH,
                                dialogHeight, parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    auto* state =
        new InputDialogState{std::move(validator), std::move(onConfirm), std::move(onCancel)};
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int contentW = DIALOG_WIDTH - 48;

    if (hasLabel) {
        HWND lbl = CreateWindowExW(0, L"STATIC", toWide(label).c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_LEFT, 16, labelY, contentW, 20, hwnd,
                                   (HMENU)(INT_PTR)ID_LABEL, nullptr, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    }

    state->editControl =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", toWide(initialValue).c_str(),
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 16, editY, contentW,
                        24, hwnd, (HMENU)(INT_PTR)ID_EDIT, nullptr, nullptr);
    SendMessageW(state->editControl, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(state->editControl, EM_SETSEL, 0, -1);

    state->errorLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 16, errorY, contentW,
                                        36, hwnd, (HMENU)(INT_PTR)ID_ERROR, nullptr, nullptr);
    SendMessageW(state->errorLabel, WM_SETFONT, (WPARAM)font, TRUE);

    HWND confirmBtn =
        CreateWindowExW(0, L"BUTTON", toWide(confirmButtonText).c_str(),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, contentW - 84,
                        buttonY, 100, 28, hwnd, (HMENU)(INT_PTR)ID_CONFIRM, nullptr, nullptr);
    SendMessageW(confirmBtn, WM_SETFONT, (WPARAM)font, TRUE);

    HWND cancelBtn = CreateWindowExW(
        0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, contentW - 188,
        buttonY, 100, 28, hwnd, (HMENU)(INT_PTR)ID_CANCEL, nullptr, nullptr);
    SendMessageW(cancelBtn, WM_SETFONT, (WPARAM)font, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(state->editControl);

    if (parent)
        EnableWindow(parent, FALSE);

    runModalLoop(hwnd);

    if (parent) {
        EnableWindow(parent, TRUE);
        SetActiveWindow(parent);
        SetForegroundWindow(parent);
    }

    delete state;
}

#endif
