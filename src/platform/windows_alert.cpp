#if defined(_WIN32)

#include "application.hpp"
#include "platform/alert.hpp"
#include "platform/windows_platform.hpp"
#include "themes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dwmapi.h>
#include <windows.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr int kButtonBaseId = 100;
    constexpr int kDialogMinWidth = 360;
    constexpr int kDialogPreferredWidth = 440;
    constexpr int kDialogMaxWidth = 520;
    constexpr int kDialogPadding = 20;
    constexpr int kButtonHeight = 32;
    constexpr int kButtonMinWidth = 92;
    constexpr int kButtonHorizontalPadding = 28;
    constexpr int kButtonGap = 8;
    constexpr int kTitleBottomGap = 12;
    constexpr int kMessageBottomGap = 18;

    struct AlertButtonLayout {
        int controlId = 0;
        int buttonIndex = -1;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = kButtonHeight;
        bool isDefault = false;
    };

    struct AlertDialogState {
        std::wstring title;
        std::wstring message;
        std::vector<AlertButton> buttons;
        std::vector<std::wstring> wideLabels;
        std::vector<AlertButtonLayout> layoutButtons;
        Theme::Colors colors = Theme::NATIVE_DARK;
        HWND parent = nullptr;
        HFONT bodyFont = nullptr;
        HFONT titleFont = nullptr;
        bool isDark = true;
        int defaultIndex = 0;
        int cancelIndex = -1;
        int chosenIndex = -1;
        int clientWidth = 0;
        int clientHeight = 0;
        RECT titleRect = {};
        RECT messageRect = {};
    };

    auto toWide(const std::string& s) -> std::wstring {
        if (s.empty()) {
            return {};
        }

        const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring wide(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wide.data(), len);
        return wide;
    }

    COLORREF toColorRef(const ImVec4& color) {
        const auto clampChannel = [](float value) -> BYTE {
            const float scaled = std::clamp(value, 0.0f, 1.0f) * 255.0f;
            return static_cast<BYTE>(scaled + 0.5f);
        };
        return RGB(clampChannel(color.x), clampChannel(color.y), clampChannel(color.z));
    }

    ImVec4 blendColor(const ImVec4& background, const ImVec4& foreground, float amount) {
        const float clampedAmount = std::clamp(amount, 0.0f, 1.0f);
        return ImVec4(background.x + (foreground.x - background.x) * clampedAmount,
                      background.y + (foreground.y - background.y) * clampedAmount,
                      background.z + (foreground.z - background.z) * clampedAmount, 1.0f);
    }

    HFONT getDialogFont() {
        static HFONT font = nullptr;
        if (!font) {
            NONCLIENTMETRICSW metrics = {sizeof(metrics)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            font = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        return font;
    }

    HFONT getDialogTitleFont() {
        static HFONT font = nullptr;
        if (!font) {
            NONCLIENTMETRICSW metrics = {sizeof(metrics)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
            metrics.lfMessageFont.lfHeight =
                static_cast<LONG>(metrics.lfMessageFont.lfHeight * 1.25f);
            font = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        return font;
    }

    RECT measureWrappedText(HDC dc, HFONT font, const std::wstring& text, int width, UINT flags) {
        RECT rect{0, 0, width, 0};
        const HGDIOBJ oldFont = SelectObject(dc, font);
        DrawTextW(dc, text.c_str(), -1, &rect, flags | DT_CALCRECT);
        SelectObject(dc, oldFont);
        return rect;
    }

    int getButtonWidth(HDC dc, HFONT font, const std::wstring& label) {
        const HGDIOBJ oldFont = SelectObject(dc, font);
        SIZE textSize{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &textSize);
        SelectObject(dc, oldFont);
        return std::max(kButtonMinWidth, static_cast<int>(textSize.cx) + kButtonHorizontalPadding);
    }

    int chooseCloseIndex(const AlertDialogState& state) {
        if (state.cancelIndex >= 0) {
            return state.cancelIndex;
        }
        if (state.defaultIndex >= 0) {
            return state.defaultIndex;
        }
        return state.buttons.empty() ? -1 : 0;
    }

    void drawCenteredText(HDC dc, const RECT& rect, const std::wstring& text, COLORREF textColor,
                          HFONT font) {
        const int oldBkMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldTextColor = SetTextColor(dc, textColor);
        const HGDIOBJ oldFont = SelectObject(dc, font);
        DrawTextW(dc, text.c_str(), -1, const_cast<RECT*>(&rect),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        SetTextColor(dc, oldTextColor);
        SetBkMode(dc, oldBkMode);
    }

    void drawRoundedButton(HDC dc, const RECT& rect, COLORREF fillColor, COLORREF borderColor) {
        HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
        HBRUSH brush = CreateSolidBrush(fillColor);
        const HGDIOBJ oldPen = SelectObject(dc, pen);
        const HGDIOBJ oldBrush = SelectObject(dc, brush);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void drawButton(const AlertDialogState& state, const DRAWITEMSTRUCT* drawInfo) {
        const auto it =
            std::find_if(state.layoutButtons.begin(), state.layoutButtons.end(),
                         [drawInfo](const AlertButtonLayout& layout) {
                             return layout.controlId == static_cast<int>(drawInfo->CtlID);
                         });
        if (it == state.layoutButtons.end()) {
            return;
        }

        const auto& button = state.buttons[it->buttonIndex];
        const auto& colors = state.colors;
        const bool isPressed = (drawInfo->itemState & ODS_SELECTED) != 0;
        const bool isFocused = (drawInfo->itemState & ODS_FOCUS) != 0;
        const bool isDisabled = (drawInfo->itemState & ODS_DISABLED) != 0;
        const bool isHot = (drawInfo->itemState & ODS_HOTLIGHT) != 0;

        ImVec4 fill = colors.surface0;
        ImVec4 border = colors.overlay0;
        ImVec4 text = colors.text;

        if (button.style == AlertButton::Style::Default) {
            fill = isPressed ? colors.sky : (isHot ? colors.sapphire : colors.blue);
            border = fill;
            text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        } else if (button.style == AlertButton::Style::Destructive) {
            fill = blendColor(colors.base, colors.red, isPressed ? 0.32f : (isHot ? 0.24f : 0.18f));
            border = blendColor(colors.base, colors.red, isPressed ? 0.55f : 0.40f);
            text = colors.red;
        } else {
            fill = isPressed ? colors.surface2 : (isHot ? colors.surface1 : colors.surface0);
            border = isPressed ? colors.overlay1 : colors.overlay0;
        }

        if (isDisabled) {
            fill = colors.surface0;
            border = colors.overlay0;
            text = colors.subtext0;
        }

        drawRoundedButton(drawInfo->hDC, drawInfo->rcItem, toColorRef(fill), toColorRef(border));
        drawCenteredText(drawInfo->hDC, drawInfo->rcItem, state.wideLabels[it->buttonIndex],
                         toColorRef(text), state.bodyFont);

        if (isFocused) {
            RECT focusRect = drawInfo->rcItem;
            InflateRect(&focusRect, -4, -4);
            DrawFocusRect(drawInfo->hDC, &focusRect);
        }
    }

    LRESULT CALLBACK alertDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* state = reinterpret_cast<AlertDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }

        case WM_CREATE: {
            if (!state) {
                return -1;
            }

            const BOOL useDarkMode = state->isDark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode,
                                  sizeof(useDarkMode));

            for (const auto& layout : state->layoutButtons) {
                DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
                if (layout.isDefault) {
                    style |= BS_DEFPUSHBUTTON;
                } else {
                    style |= BS_PUSHBUTTON;
                }

                HWND button =
                    CreateWindowExW(0, L"BUTTON", state->wideLabels[layout.buttonIndex].c_str(),
                                    style, layout.x, layout.y, layout.width, layout.height, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(layout.controlId)),
                                    GetModuleHandleW(nullptr), nullptr);
                SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state->bodyFont), TRUE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            if (!state) {
                break;
            }

            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            HBRUSH bgBrush = CreateSolidBrush(toColorRef(state->colors.base));
            FillRect(dc, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            const int oldBkMode = SetBkMode(dc, TRANSPARENT);
            const COLORREF oldTextColor = SetTextColor(dc, toColorRef(state->colors.text));

            HGDIOBJ oldFont = SelectObject(dc, state->titleFont);
            RECT titleRect = state->titleRect;
            DrawTextW(dc, state->title.c_str(), -1, &titleRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

            if (!state->message.empty()) {
                SelectObject(dc, state->bodyFont);
                SetTextColor(dc, toColorRef(state->colors.subtext1));
                RECT messageRect = state->messageRect;
                DrawTextW(dc, state->message.c_str(), -1, &messageRect,
                          DT_LEFT | DT_TOP | DT_WORDBREAK);
            }

            SelectObject(dc, state->bodyFont);
            HPEN separatorPen = CreatePen(PS_SOLID, 1, toColorRef(state->colors.overlay0));
            HGDIOBJ oldPen = SelectObject(dc, separatorPen);
            const int separatorY =
                state->layoutButtons.empty()
                    ? state->clientHeight - kDialogPadding - kButtonHeight - kMessageBottomGap / 2
                    : state->layoutButtons.front().y - (kMessageBottomGap / 2);
            MoveToEx(dc, kDialogPadding, separatorY, nullptr);
            LineTo(dc, state->clientWidth - kDialogPadding, separatorY);
            SelectObject(dc, oldPen);
            DeleteObject(separatorPen);

            SelectObject(dc, oldFont);
            SetTextColor(dc, oldTextColor);
            SetBkMode(dc, oldBkMode);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM:
            if (state) {
                drawButton(*state, reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;

        case WM_COMMAND: {
            if (!state || HIWORD(wParam) != BN_CLICKED) {
                break;
            }

            const int controlId = LOWORD(wParam);
            const auto it = std::find_if(state->layoutButtons.begin(), state->layoutButtons.end(),
                                         [controlId](const AlertButtonLayout& layout) {
                                             return layout.controlId == controlId;
                                         });
            if (it != state->layoutButtons.end()) {
                state->chosenIndex = it->buttonIndex;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            if (state) {
                state->chosenIndex = chooseCloseIndex(*state);
            }
            DestroyWindow(hwnd);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void registerAlertDialogClass() {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = alertDialogProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"DearSQLAlertDialog";
        RegisterClassExW(&windowClass);

        registered = true;
    }

    void runModalLoop(HWND hwnd) {
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
} // namespace

void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty()) {
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    }

    registerAlertDialogClass();

    auto* platform = dynamic_cast<WindowsPlatform*>(Application::getInstance().getPlatform());
    HWND parent = platform ? platform->getHWND() : nullptr;

    auto state = std::make_unique<AlertDialogState>();
    state->title = toWide(title);
    state->message = toWide(message);
    state->buttons = std::move(buttons);
    state->parent = parent;
    state->colors = Application::getInstance().getCurrentColors();
    state->isDark = Application::getInstance().isDarkTheme();
    state->bodyFont = getDialogFont();
    state->titleFont = getDialogTitleFont();

    for (const auto& button : state->buttons) {
        state->wideLabels.push_back(toWide(button.text));
    }

    for (size_t i = 0; i < state->buttons.size(); ++i) {
        if (state->buttons[i].style == AlertButton::Style::Default) {
            state->defaultIndex = static_cast<int>(i);
            break;
        }
    }

    for (size_t i = 0; i < state->buttons.size(); ++i) {
        if (state->buttons[i].style == AlertButton::Style::Cancel) {
            state->cancelIndex = static_cast<int>(i);
            break;
        }
    }

    HDC measureDc = GetDC(parent);
    if (!measureDc) {
        measureDc = GetDC(nullptr);
    }

    const int contentWidth =
        std::clamp(kDialogPreferredWidth - kDialogPadding * 2, kDialogMinWidth - kDialogPadding * 2,
                   kDialogMaxWidth - kDialogPadding * 2);
    const RECT titleBounds = measureWrappedText(measureDc, state->titleFont, state->title,
                                                contentWidth, DT_LEFT | DT_TOP | DT_WORDBREAK);
    const RECT messageBounds =
        state->message.empty() ? RECT{0, 0, contentWidth, 0}
                               : measureWrappedText(measureDc, state->bodyFont, state->message,
                                                    contentWidth, DT_LEFT | DT_TOP | DT_WORDBREAK);

    std::vector<int> buttonWidths;
    buttonWidths.reserve(state->buttons.size());

    int buttonRowWidth = 0;
    for (size_t i = 0; i < state->buttons.size(); ++i) {
        const int buttonWidth = getButtonWidth(measureDc, state->bodyFont, state->wideLabels[i]);
        buttonWidths.push_back(buttonWidth);
        buttonRowWidth += buttonWidth;
        if (i > 0) {
            buttonRowWidth += kButtonGap;
        }
    }

    if (measureDc) {
        ReleaseDC(parent ? parent : nullptr, measureDc);
    }

    state->clientWidth =
        std::clamp(std::max(kDialogPreferredWidth, buttonRowWidth + kDialogPadding * 2),
                   kDialogMinWidth, kDialogMaxWidth);

    const int titleHeight = std::max(28, static_cast<int>(titleBounds.bottom - titleBounds.top));
    const int messageHeight =
        state->message.empty() ? 0 : static_cast<int>(messageBounds.bottom - messageBounds.top);

    int contentBottom = kDialogPadding + titleHeight + kTitleBottomGap;
    if (messageHeight > 0) {
        contentBottom += messageHeight + kMessageBottomGap;
    } else {
        contentBottom += kMessageBottomGap / 2;
    }

    state->clientHeight = contentBottom + kButtonHeight + kDialogPadding;
    state->titleRect = {kDialogPadding, kDialogPadding, state->clientWidth - kDialogPadding,
                        kDialogPadding + titleHeight};

    if (messageHeight > 0) {
        const int messageTop = state->titleRect.bottom + kTitleBottomGap;
        state->messageRect = {kDialogPadding, messageTop, state->clientWidth - kDialogPadding,
                              messageTop + messageHeight};
    }

    int buttonX = state->clientWidth - kDialogPadding - buttonRowWidth;
    const int buttonY = state->clientHeight - kDialogPadding - kButtonHeight;
    for (size_t i = 0; i < state->buttons.size(); ++i) {
        state->layoutButtons.push_back({
            .controlId = kButtonBaseId + static_cast<int>(i),
            .buttonIndex = static_cast<int>(i),
            .x = buttonX,
            .y = buttonY,
            .width = buttonWidths[i],
            .height = kButtonHeight,
            .isDefault = static_cast<int>(i) == state->defaultIndex,
        });
        buttonX += buttonWidths[i] + kButtonGap;
    }

    RECT windowRect{0, 0, state->clientWidth, state->clientHeight};
    const DWORD style = WS_CAPTION | WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);

    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;

    int posX = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;
    if (parent) {
        RECT parentRect{};
        GetWindowRect(parent, &parentRect);
        posX = parentRect.left + ((parentRect.right - parentRect.left) - windowWidth) / 2;
        posY = parentRect.top + ((parentRect.bottom - parentRect.top) - windowHeight) / 2;
    }

    HWND hwnd = CreateWindowExW(exStyle, L"DearSQLAlertDialog", state->title.c_str(), style, posX,
                                posY, windowWidth, windowHeight, parent, nullptr,
                                GetModuleHandleW(nullptr), state.get());
    if (!hwnd) {
        const int result = MessageBoxW(parent, state->message.c_str(), state->title.c_str(),
                                       state->cancelIndex >= 0 ? MB_OKCANCEL : MB_OK);
        const int fallbackIndex = (result == IDCANCEL && state->cancelIndex >= 0)
                                      ? state->cancelIndex
                                      : chooseCloseIndex(*state);
        if (fallbackIndex >= 0 && fallbackIndex < static_cast<int>(state->buttons.size()) &&
            state->buttons[fallbackIndex].onPress) {
            state->buttons[fallbackIndex].onPress();
        }
        return;
    }

    if (parent) {
        EnableWindow(parent, FALSE);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    if (HWND defaultButton = GetDlgItem(hwnd, kButtonBaseId + state->defaultIndex)) {
        SetFocus(defaultButton);
    }

    runModalLoop(hwnd);

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    const int chosenIndex = state->chosenIndex;
    std::function<void()> onPress;
    if (chosenIndex >= 0 && chosenIndex < static_cast<int>(state->buttons.size())) {
        onPress = std::move(state->buttons[chosenIndex].onPress);
    }

    if (onPress) {
        onPress();
    }
}

#endif
