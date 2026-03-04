#if defined(_WIN32)

#include "application.hpp"
#include "platform/alert.hpp"
#include "platform/windows_platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <commctrl.h>
#include <windows.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#pragma comment(lib, "comctl32.lib")

void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty()) {
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    }

    // get parent HWND
    HWND parent = nullptr;
    auto* platform = dynamic_cast<WindowsPlatform*>(Application::getInstance().getPlatform());
    if (platform) {
        parent = platform->getHWND();
    }

    // build TaskDialog button array
    std::vector<std::wstring> wideLabels;
    wideLabels.reserve(buttons.size());
    for (const auto& btn : buttons) {
        int len = MultiByteToWideChar(CP_UTF8, 0, btn.text.c_str(), -1, nullptr, 0);
        std::wstring wide(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, btn.text.c_str(), -1, wide.data(), len);
        wideLabels.push_back(std::move(wide));
    }

    std::vector<TASKDIALOG_BUTTON> tdButtons;
    tdButtons.reserve(buttons.size());
    for (size_t i = 0; i < buttons.size(); ++i) {
        TASKDIALOG_BUTTON tdb = {};
        tdb.nButtonID = static_cast<int>(100 + i);
        tdb.pszButtonText = wideLabels[i].c_str();
        tdButtons.push_back(tdb);
    }

    // convert title and message to wide strings
    auto toWide = [](const std::string& s) -> std::wstring {
        if (s.empty())
            return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring wide(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wide.data(), len);
        return wide;
    };

    std::wstring wideTitle = toWide(title);
    std::wstring wideMessage = toWide(message);

    // find default and cancel button IDs
    int defaultButtonId = 0;
    int cancelButtonId = 0;
    for (size_t i = 0; i < buttons.size(); ++i) {
        int id = static_cast<int>(100 + i);
        if (buttons[i].style == AlertButton::Style::Default && defaultButtonId == 0) {
            defaultButtonId = id;
        }
        if (buttons[i].style == AlertButton::Style::Cancel && cancelButtonId == 0) {
            cancelButtonId = id;
        }
    }
    if (defaultButtonId == 0 && !tdButtons.empty()) {
        defaultButtonId = tdButtons[0].nButtonID;
    }

    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.hwndParent = parent;
    config.dwFlags = TDF_USE_COMMAND_LINKS_NO_ICON;
    config.pszWindowTitle = wideTitle.c_str();
    config.pszMainInstruction = wideTitle.c_str();
    if (!wideMessage.empty()) {
        config.pszContent = wideMessage.c_str();
    }
    config.cButtons = static_cast<UINT>(tdButtons.size());
    config.pButtons = tdButtons.data();
    config.nDefaultButton = defaultButtonId;

    // use simple buttons instead of command links for short labels
    config.dwFlags = 0;
    if (cancelButtonId != 0) {
        config.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;
    }

    int clickedButton = 0;
    HRESULT hr = TaskDialogIndirect(&config, &clickedButton, nullptr, nullptr);

    if (SUCCEEDED(hr)) {
        int index = clickedButton - 100;
        if (index >= 0 && index < static_cast<int>(buttons.size())) {
            if (buttons[index].onPress) {
                buttons[index].onPress();
            }
        } else if (cancelButtonId != 0) {
            // dialog was closed via X or Escape — treat as cancel
            int cancelIndex = cancelButtonId - 100;
            if (cancelIndex >= 0 && cancelIndex < static_cast<int>(buttons.size())) {
                if (buttons[cancelIndex].onPress) {
                    buttons[cancelIndex].onPress();
                }
            }
        }
    } else {
        // TaskDialogIndirect failed, fall back to MessageBox
        int mbType = MB_OK;
        if (buttons.size() == 2) {
            mbType = MB_OKCANCEL;
        }
        std::wstring fullMsg = wideTitle;
        if (!wideMessage.empty()) {
            fullMsg += L"\n\n" + wideMessage;
        }
        int result = MessageBoxW(parent, fullMsg.c_str(), wideTitle.c_str(), mbType);

        // map MessageBox result to first matching button
        if (result == IDOK && !buttons.empty()) {
            for (size_t i = 0; i < buttons.size(); ++i) {
                if (buttons[i].style == AlertButton::Style::Default) {
                    if (buttons[i].onPress)
                        buttons[i].onPress();
                    break;
                }
            }
        }
    }
}

#endif
