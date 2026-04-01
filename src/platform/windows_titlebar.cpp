#if defined(_WIN32)

#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "platform/alert.hpp"
#include "platform/titlebar.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "ui/input_dialog.hpp"

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

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

WindowsTitlebar::WindowsTitlebar(Application* app, GLFWwindow* window)
    : app_(app), window_(window) {
    lastAppliedDarkTheme_ = app ? app->isDarkTheme() : true;
}

void WindowsTitlebar::setup() {
    applyTitlebarTheme();
    std::cout << "Windows custom titlebar configured" << std::endl;
}

void WindowsTitlebar::applyTitlebarTheme() {
    if (!window_ || !app_) {
        return;
    }

    HWND hWnd = glfwGetWin32Window(window_);
    if (!hWnd) {
        return;
    }

    const bool isDark = app_->isDarkTheme();
    lastAppliedDarkTheme_ = isDark;

    const BOOL useDarkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
}

void WindowsTitlebar::applyTheme(bool isDark) {
    if (isDark != lastAppliedDarkTheme_) {
        lastAppliedDarkTheme_ = isDark;
        themeChanged_ = true;
        applyTitlebarTheme();
    }
}

int WindowsTitlebar::getTitlebarHeightPixels() const {
    return 32;
}

int WindowsTitlebar::getResizeBorderWidth() const {
    return GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

LRESULT WindowsTitlebar::hitTest(HWND hWnd, LPARAM lParam) const {
    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    RECT rc;
    GetWindowRect(hWnd, &rc);

    const int border = getResizeBorderWidth();
    const int titlebar = getTitlebarHeightPixels();
    const bool maximized = IsZoomed(hWnd);

    // resize borders (disabled when maximized)
    if (!maximized) {
        // top edge
        if (pt.y >= rc.top && pt.y < rc.top + border) {
            if (pt.x < rc.left + border)
                return HTTOPLEFT;
            if (pt.x >= rc.right - border)
                return HTTOPRIGHT;
            return HTTOP;
        }
        // bottom edge
        if (pt.y >= rc.bottom - border) {
            if (pt.x < rc.left + border)
                return HTBOTTOMLEFT;
            if (pt.x >= rc.right - border)
                return HTBOTTOMRIGHT;
            return HTBOTTOM;
        }
        // left edge
        if (pt.x >= rc.left && pt.x < rc.left + border)
            return HTLEFT;
        // right edge
        if (pt.x >= rc.right - border)
            return HTRIGHT;
    }

    // titlebar area
    if (pt.y < rc.top + titlebar) {
        // caption buttons zone (right side: 3 buttons * 46px)
        if (pt.x >= rc.right - 138) {
            return HTCLIENT;
        }
        // left interactive zone (sidebar toggle, add button, etc.)
        if (pt.x < rc.left + static_cast<int>(interactiveLeftEnd_)) {
            return HTCLIENT;
        }
        // right interactive zone (workspace dropdown, menu button)
        if (interactiveRightStart_ > 0 &&
            pt.x >= rc.left + static_cast<int>(interactiveRightStart_)) {
            return HTCLIENT;
        }
        return HTCAPTION;
    }

    // everything else is normal client area
    return HTCLIENT;
}

// Icon draw helpers
namespace {
    bool isWindowsAppsUseDarkTheme() {
        DWORD value = 1;
        DWORD valueSize = sizeof(value);
        const LSTATUS status = RegGetValueW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &valueSize);
        return status == ERROR_SUCCESS ? value == 0 : false;
    }

    void DrawMinimizeIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
        dl->AddLine({center.x - 5, center.y}, {center.x + 5, center.y}, col, 1.0f);
    }

    void DrawMaximizeIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
        dl->AddRect({center.x - 5, center.y - 5}, {center.x + 5, center.y + 5}, col, 0, 0, 1.0f);
    }

    void DrawRestoreIcon(ImDrawList* dl, ImVec2 center, ImU32 col, ImU32 bgCol) {
        const float s = 4.0f, off = 2.0f;
        // back rectangle (top-right, partially occluded)
        dl->AddRect({center.x - s + off, center.y - s - off},
                    {center.x + s + off, center.y + s - off}, col, 0, 0, 1.0f);
        // front rectangle (bottom-left) — fill to occlude back rect lines
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

void WindowsTitlebar::render() {
    const float tbHeight = static_cast<float>(getTitlebarHeightPixels());
    if (tbHeight <= 0)
        return;

    HWND hWnd = glfwGetWin32Window(window_);

    const bool isDark = app_->isDarkTheme();
    if (isDark != lastAppliedDarkTheme_) {
        applyTheme(isDark);
    }
    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    const bool maximized = IsZoomed(hWnd);

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin = vp->Pos;
    const float winW = vp->Size.x;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // background
    fg->AddRectFilled(origin, {origin.x + winW, origin.y + tbHeight},
                      ImGui::GetColorU32(colors.mantle));

    titlebarWidgetHovered_ = false;

    const ImVec4 hoverBg = isDark ? ImVec4(1, 1, 1, 0.1f) : ImVec4(0, 0, 0, 0.06f);
    const ImVec4 activeBg = isDark ? ImVec4(1, 1, 1, 0.15f) : ImVec4(0, 0, 0, 0.10f);

    // helper: manual foreground button (no ImGui window context needed)
    auto fgButton = [&](ImVec2 bMin, ImVec2 bMax, auto drawIcon, ImVec4 hBg, ImVec4 aBg,
                        bool whiteIconOnHover) -> bool {
        bool hovered =
            (mouse.x >= bMin.x && mouse.x < bMax.x && mouse.y >= bMin.y && mouse.y < bMax.y);
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

    // helper: simple icon button (rounded, fixed size)
    const float iconBtnSize = tbHeight - 4.0f;
    auto fgIconBtn = [&](float x, const char* iconText) -> bool {
        float iy = origin.y + (tbHeight - iconBtnSize) * 0.5f;
        ImVec2 bMin{x, iy}, bMax{x + iconBtnSize, iy + iconBtnSize};
        bool hovered =
            (mouse.x >= bMin.x && mouse.x < bMax.x && mouse.y >= bMin.y && mouse.y < bMax.y);
        bool held = hovered && io.MouseDown[0];
        bool clicked = hovered && io.MouseReleased[0];
        titlebarWidgetHovered_ |= hovered;

        if (held)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(activeBg), 4.0f);
        else if (hovered)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(hoverBg), 4.0f);

        // center icon text
        ImVec2 textSize = ImGui::CalcTextSize(iconText);
        fg->AddText({(bMin.x + bMax.x - textSize.x) * 0.5f, (bMin.y + bMax.y - textSize.y) * 0.5f},
                    ImGui::GetColorU32(colors.text), iconText);

        return clicked;
    };

    // RIGHT SIDE: Caption buttons
    const float captionBtnW = 46.0f;
    const ImVec4 closeHoverBg = ImVec4(0.77f, 0.17f, 0.11f, 1.0f);
    const ImVec4 closeActiveBg = ImVec4(0.67f, 0.14f, 0.09f, 1.0f);

    float rx = origin.x + winW - captionBtnW * 3;

    if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight}, DrawMinimizeIcon, hoverBg,
                 activeBg, false))
        ShowWindow(hWnd, SW_MINIMIZE);
    rx += captionBtnW;

    if (maximized) {
        if (fgButton(
                {rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight},
                [&](ImDrawList* d, ImVec2 c, ImU32 col) {
                    DrawRestoreIcon(d, c, col, ImGui::GetColorU32(colors.mantle));
                },
                hoverBg, activeBg, false))
            ShowWindow(hWnd, SW_RESTORE);
    } else {
        if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight}, DrawMaximizeIcon,
                     hoverBg, activeBg, false))
            ShowWindow(hWnd, SW_MAXIMIZE);
    }
    rx += captionBtnW;

    if (fgButton({rx, origin.y}, {rx + captionBtnW, origin.y + tbHeight}, DrawCloseIcon,
                 closeHoverBg, closeActiveBg, true))
        PostMessageW(hWnd, WM_CLOSE, 0, 0);

    // RIGHT SIDE: Menu + Workspace (before caption)
    const float rightGroupX =
        origin.x + winW - captionBtnW * 3 - 8.0f; // 8px gap before caption btns

    // menu button (rightmost before caption buttons)
    float menuX = rightGroupX - iconBtnSize;
    if (fgIconBtn(menuX, ICON_FA_ELLIPSIS_VERTICAL)) {
        openMenuPopup_ = true;
        menuPopupPos_ = {menuX, origin.y + tbHeight};
    }

    // workspace dropdown
    const std::string wsName = app_->getCurrentWorkspaceName();
    const std::string wsLabel = wsName + "  " ICON_FA_CHEVRON_DOWN;
    ImVec2 wsTextSize = ImGui::CalcTextSize(wsLabel.c_str());
    float wsBtnW = wsTextSize.x + 16.0f;
    float wsX = menuX - 4.0f - wsBtnW;
    {
        float iy = origin.y + (tbHeight - iconBtnSize) * 0.5f;
        ImVec2 bMin{wsX, iy}, bMax{wsX + wsBtnW, iy + iconBtnSize};
        bool hovered =
            (mouse.x >= bMin.x && mouse.x < bMax.x && mouse.y >= bMin.y && mouse.y < bMax.y);
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

    // track right interactive zone start for hit testing
    interactiveRightStart_ = wsX;

    // LEFT SIDE: Sidebar toggle + Add connection
    const float leftPad = 8.0f;
    float lx = origin.x + leftPad;

    // sidebar toggle (hamburger icon drawn manually)
    {
        float iy = origin.y + (tbHeight - iconBtnSize) * 0.5f;
        ImVec2 bMin{lx, iy}, bMax{lx + iconBtnSize, iy + iconBtnSize};
        bool hovered =
            (mouse.x >= bMin.x && mouse.x < bMax.x && mouse.y >= bMin.y && mouse.y < bMax.y);
        bool held = hovered && io.MouseDown[0];
        bool clicked = hovered && io.MouseReleased[0];
        titlebarWidgetHovered_ |= hovered;
        if (held)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(activeBg), 4.0f);
        else if (hovered)
            fg->AddRectFilled(bMin, bMax, ImGui::GetColorU32(hoverBg), 4.0f);

        ImVec2 center{(bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f};
        ImU32 iconCol = ImGui::GetColorU32(colors.text);
        fg->AddLine({center.x - 5, center.y - 4}, {center.x + 5, center.y - 4}, iconCol, 1.5f);
        fg->AddLine({center.x - 5, center.y}, {center.x + 5, center.y}, iconCol, 1.5f);
        fg->AddLine({center.x - 5, center.y + 4}, {center.x + 5, center.y + 4}, iconCol, 1.5f);

        if (clicked)
            app_->setSidebarVisible(!app_->isSidebarVisible());
    }
    lx += iconBtnSize + 2.0f;

    // add connection button (+)
    if (fgIconBtn(lx, ICON_FA_PLUS)) {
        if (app_->getDatabaseSidebar()) {
            app_->getDatabaseSidebar()->showConnectionDialog();
        }
    }
    lx += iconBtnSize + 8.0f;

    // track left interactive zone end for hit testing
    interactiveLeftEnd_ = lx;

    // title text
    const float titleY = origin.y + (tbHeight - ImGui::GetFontSize()) * 0.5f;
#ifdef NDEBUG
    fg->AddText({lx, titleY}, ImGui::GetColorU32(colors.subtext1), APP_NAME);
#else
    fg->AddText({lx, titleY}, ImGui::GetColorU32(colors.subtext1), "DearSQL (Debug)");
#endif
}

void WindowsTitlebar::renderPopups() {
    if (!app_)
        return;

    const bool isDark = app_->isDarkTheme();
    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    const auto workspaces = app_->getWorkspaces();
    constexpr const char* newWorkspaceLabel = "  " ICON_FA_PLUS "  New Workspace...";

    // we need an ImGui window context to host popups
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

    // workspace popup
    if (openWorkspacePopup_) {
        ImGui::OpenPopup("##WorkspacePopup");
        openWorkspacePopup_ = false;
    }
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const float popupPadding = 8.0f;
    const float rowPaddingX = 8.0f;
    const float rowHeight = std::max(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight() + 10.0f);
    const float actionBtnSize = rowHeight - 6.0f;
    const float actionGap = 4.0f;
    const float actionAreaW = actionBtnSize * 2.0f + actionGap;
    const float checkSlotW = ImGui::CalcTextSize(ICON_FA_CHECK).x + rowPaddingX + 6.0f;

    float widestLabelW = ImGui::CalcTextSize(newWorkspaceLabel).x;
    for (const auto& ws : workspaces) {
        widestLabelW = std::max(widestLabelW, ImGui::CalcTextSize(ws.name.c_str()).x);
    }

    const float minPopupW = 220.0f;
    const float maxPopupW = std::max(minPopupW, std::min(420.0f, mainViewport->WorkSize.x - 16.0f));
    const float contentW =
        std::clamp(checkSlotW + widestLabelW + actionAreaW + rowPaddingX * 3.0f,
                   minPopupW - popupPadding * 2.0f, maxPopupW - popupPadding * 2.0f);
    const float workspacePopupW = contentW + popupPadding * 2.0f;
    const float popupMinX = mainViewport->WorkPos.x + 8.0f;
    const float popupMaxX = std::max(popupMinX, mainViewport->WorkPos.x + mainViewport->WorkSize.x -
                                                    workspacePopupW - 8.0f);
    const float popupX = std::clamp(workspacePopupPos_.x, popupMinX, popupMaxX);

    ImGui::SetNextWindowPos({popupX, workspacePopupPos_.y});
    ImGui::SetNextWindowSize({workspacePopupW, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 2});
    if (ImGui::BeginPopup("##WorkspacePopup")) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 hoverRowBg = ImGui::GetColorU32(colors.surface1);
        const ImU32 textColor = ImGui::GetColorU32(colors.text);

        for (const auto& ws : workspaces) {
            bool isCurrent = (ws.id == app_->getCurrentWorkspaceId());
            ImGui::PushID(ws.id);

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            ImGui::Dummy({contentW, rowHeight});
            const ImVec2 nextRowPos = ImGui::GetCursorScreenPos();
            const ImVec2 rowMax{rowMin.x + contentW, rowMin.y + rowHeight};
            const bool rowHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax);

            if (rowHovered) {
                drawList->AddRectFilled(rowMin, rowMax, hoverRowBg, 4.0f);
            }

            const float actionX = rowMax.x - rowPaddingX - actionAreaW;
            const float selectX = rowMin.x + rowPaddingX;
            const float selectW = actionX - rowPaddingX - selectX;
            const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
            const float labelX = selectX + checkSlotW;
            const float labelMaxX = actionX - rowPaddingX;

            // name — clicking selects the workspace
            ImGui::SetCursorScreenPos({selectX, rowMin.y});
            if (ImGui::InvisibleButton("##select", {selectW, rowHeight})) {
                app_->setCurrentWorkspace(ws.id);
                ImGui::CloseCurrentPopup();
            }

            if (isCurrent) {
                drawList->AddText({selectX, textY}, textColor, ICON_FA_CHECK);
            }

            drawList->PushClipRect({labelX, rowMin.y}, {labelMaxX, rowMax.y}, true);
            drawList->AddText({labelX, textY}, textColor, ws.name.c_str());
            drawList->PopClipRect();

            // action buttons — only visible on hover
            if (rowHovered) {
                const float actionY = rowMin.y + (rowHeight - actionBtnSize) * 0.5f;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface2);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay0);

                ImGui::SetCursorScreenPos({actionX, actionY});
                if (ImGui::Button(ICON_FA_PENCIL "##edit", {actionBtnSize, actionBtnSize})) {
                    const int wsId = ws.id;
                    const std::string wsName = ws.name;
                    ImGui::CloseCurrentPopup();
                    InputDialog::show("Rename Workspace", "", wsName, "Rename",
                                      [this, wsId](const std::string& newName) -> std::string {
                                          if (newName.empty())
                                              return "Name cannot be empty.";
                                          app_->renameWorkspace(wsId, newName);
                                          return "";
                                      });
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Rename");

                ImGui::PopStyleColor(3);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.77f, 0.17f, 0.11f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.67f, 0.14f, 0.09f, 0.8f));
                ImGui::SetCursorScreenPos({actionX + actionBtnSize + actionGap, actionY});
                if (ImGui::Button(ICON_FA_TRASH "##del", {actionBtnSize, actionBtnSize})) {
                    const int wsId = ws.id;
                    ImGui::CloseCurrentPopup();
                    Alert::show(
                        "Delete Workspace",
                        "Are you sure you want to delete this workspace? "
                        "All connections in this workspace will be removed.",
                        {AlertButton{"Delete", [this, wsId]() { app_->deleteWorkspace(wsId); },
                                     AlertButton::Style::Destructive},
                         AlertButton{"Cancel", {}, AlertButton::Style::Cancel}});
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Delete");

                ImGui::PopStyleColor(3);
            }

            ImGui::SetCursorScreenPos(nextRowPos);
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Selectable(newWorkspaceLabel, false, 0, {contentW, rowHeight})) {
            if (!app_->canAddWorkspace()) {
                Alert::show(
                    "Workspace Limit Reached",
                    "Free tier is limited to 1 workspace. Activate a license to create more.");
            } else {
                InputDialog::show("New Workspace", "", "", "Create",
                                  [this](const std::string& name) -> std::string {
                                      if (name.empty())
                                          return "Name cannot be empty.";
                                      app_->createWorkspace(name, "");
                                      return "";
                                  });
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // menu popup
    if (openMenuPopup_) {
        ImGui::OpenPopup("##MenuPopup");
        openMenuPopup_ = false;
    }
    // anchor popup so its RIGHT edge aligns with the menu button's right edge
    const float popupW = 300.0f;
    const float iconBtnSz = static_cast<float>(getTitlebarHeightPixels()) - 4.0f;
    ImGui::SetNextWindowPos({menuPopupPos_.x + iconBtnSz - popupW, menuPopupPos_.y});
    ImGui::SetNextWindowSize({popupW, 0});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8, 8});
    if (ImGui::BeginPopup("##MenuPopup")) {
        const float contentW = popupW - 24.0f; // minus padding

        // theme section
        ImGui::TextColored(colors.subtext0, "Theme");
        {
            constexpr float themeGap = 8.0f;
            float btnW = (contentW - themeGap * 2.0f) / 3.0f;
            auto themeBtn = [&](const char* label, bool selected, auto action) {
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, colors.blue);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.sky);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.sapphire);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                }
                if (ImGui::Button(label, {btnW, 0}))
                    action();
                if (selected)
                    ImGui::PopStyleColor(4);
            };
            themeBtn(ICON_FA_SUN "  Light", !isDark, [&] { app_->setDarkTheme(false); });
            ImGui::SameLine();
            themeBtn(ICON_FA_MOON "  Dark", isDark, [&] { app_->setDarkTheme(true); });
            ImGui::SameLine();
            themeBtn(ICON_FA_CIRCLE_HALF_STROKE "  System", false,
                     [&] { app_->setDarkTheme(isWindowsAppsUseDarkTheme()); });
        }

        ImGui::Spacing();

        // font size section
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

        // action buttons
        if (ImGui::Selectable("Manage License...")) {
            showLicenseDialog();
        }
        if (ImGui::Selectable("Check for Updates...")) {
            checkForUpdates();
        }
        if (ImGui::Selectable("Report Bug...")) {
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/dunkbing/dearsql/issues/new"
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
// showCreateWorkspaceDialog / showLicenseDialog
// ---------------------------------------------------------------------------

void WindowsTitlebar::showCreateWorkspaceDialog() {}

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
                HWND link = CreateWindowExW(
                    0, L"SysLink",
                    L"<a "
                    L"href=\"https://buy.polar.sh/"
                    L"polar_cl_IpYdAWiNljfzsXgatypm2mg40Mm2c4hB0DcVX1L9P6p\">Purchase one</a>",
                    WS_CHILD | WS_VISIBLE, pad + 150, y, 150, labelH, hWnd,
                    reinterpret_cast<HMENU>(IDC_LICENSE_PURCHASE_LINK), GetModuleHandleW(nullptr),
                    nullptr);
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
                ShellExecuteW(
                    nullptr, L"open",
                    L"https://buy.polar.sh/polar_cl_IpYdAWiNljfzsXgatypm2mg40Mm2c4hB0DcVX1L9P6p",
                    nullptr, nullptr, SW_SHOWNORMAL);
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
