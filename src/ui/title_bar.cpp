#include "ui/title_bar.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "config.hpp"
#include "imgui_internal.h"
#include "mydear/button.hpp"
#include "mydear/native_view.hpp"
#include "mydear/title_bar.hpp"
#include "mydear/updater.hpp"
#include "platform/alert.hpp"
#include "themes.hpp"
#include "ui/input_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
    ImVec2 gPopupPos; // under the workspace button

    std::unordered_map<ImGuiID, float> gWidths; // what each button wanted last frame

    // the button's glyph: an sf symbol for the native macos button, an icon font glyph elsewhere
    constexpr const char* glyph(const char* sfSymbol, const char* faIcon) {
#ifdef __APPLE__
        (void)faIcon;
        return sfSymbol;
#else
        (void)sfSymbol;
        return faIcon;
#endif
    }

#ifdef __APPLE__
    std::unordered_map<ImGuiID, bool> gClicks; // set by the native buttons' actions

    // a native NSButton (an sf symbol, optional title) placed in the imgui bar. a real click
    // reaches the NSButton; the test engine's click reaches the imgui item under it
    bool barButton(const char* id, const char* symbol, const char* tooltip,
                   const char* title = nullptr) {
        const ImGuiID key = ImGui::GetID(id);
        bool& clicked = gClicks[key];
        const float side = ImGui::GetFrameHeight();
        const auto known = gWidths.find(key);
        const float width = known != gWidths.end()
                                ? std::max(side, known->second)
                                : side + (title ? ImGui::CalcTextSize(title).x + side : 0);
        void* view = mydear::NativeView(id, ImVec2(width, side), [&] {
            return TitleBarView::makeNativeButton(symbol, tooltip, &clicked);
        });
        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(
            ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), key, &hovered, &held);
        [[maybe_unused]] ImGuiContext& g = *GImGui; // the test engine finds items by label
        IMGUI_TEST_ENGINE_ITEM_INFO(key, id, ImGuiItemStatusFlags_None);
        if (view)
            gWidths[key] =
                TitleBarView::updateNativeButton(view, title, ImGui::GetColorU32(ImGuiCol_Text));
        return std::exchange(clicked, false) || pressed;
    }
#else
    // a flat imgui button: the icon, or the title with the icon after it
    bool barButton(const char* id, const char* icon, const char* tooltip,
                   const char* title = nullptr) {
        const std::string label =
            (title ? std::string(title) + "  " + icon : std::string(icon)) + id;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        const bool clicked = ImGui::Button(label.c_str());
        ImGui::PopStyleColor();
        gWidths[ImGui::GetItemID()] = ImGui::GetItemRectSize().x;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }
#endif

    void renderWorkspacePopup(Application& app) {
        const auto& colors = app.getCurrentColors();
        const auto workspaces = app.getWorkspaces();
        constexpr const char* newWorkspaceLabel = "  " ICON_FA_PLUS "  New Workspace...";
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const float popupPadding = 8.0f;
        const float rowPaddingX = 8.0f;
        const float rowHeight =
            std::max(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight() + 10.0f);
        const float actionBtnSize = rowHeight - 6.0f;
        const float actionGap = 4.0f;
        const float actionAreaW = actionBtnSize * 2.0f + actionGap;
        const float checkSlotW = ImGui::CalcTextSize(ICON_FA_CHECK).x + rowPaddingX + 6.0f;

        float widestLabelW = ImGui::CalcTextSize(newWorkspaceLabel).x;
        for (const auto& ws : workspaces) {
            widestLabelW = std::max(widestLabelW, ImGui::CalcTextSize(ws.name.c_str()).x);
        }

        const float minPopupW = 220.0f;
        const float maxPopupW =
            std::max(minPopupW, std::min(420.0f, mainViewport->WorkSize.x - 16.0f));
        const float contentW =
            std::clamp(checkSlotW + widestLabelW + actionAreaW + rowPaddingX * 3.0f,
                       minPopupW - popupPadding * 2.0f, maxPopupW - popupPadding * 2.0f);
        const float workspacePopupW = contentW + popupPadding * 2.0f;
        const float popupMinX = mainViewport->WorkPos.x + 8.0f;
        const float popupMaxX = std::max(
            popupMinX, mainViewport->WorkPos.x + mainViewport->WorkSize.x - workspacePopupW - 8.0f);
        const float popupX = std::clamp(gPopupPos.x, popupMinX, popupMaxX);

        ImGui::SetNextWindowPos({popupX, gPopupPos.y});
        ImGui::SetNextWindowSize({workspacePopupW, 0.0f});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.surface0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 2});
        if (ImGui::BeginPopup("##WorkspacePopup")) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 hoverRowBg = ImGui::GetColorU32(colors.surface1);
            const ImU32 textColor = ImGui::GetColorU32(colors.text);

            for (const auto& ws : workspaces) {
                bool isCurrent = (ws.id == app.getCurrentWorkspaceId());
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
                    app.setCurrentWorkspace(ws.id);
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

                    ImGui::SetCursorScreenPos({actionX, actionY});
                    if (mydear::IconButton(ICON_FA_PENCIL "##edit",
                                           mydear::ButtonVariant::Secondary,
                                           {actionBtnSize, actionBtnSize})) {
                        const int wsId = ws.id;
                        const std::string wsName = ws.name;
                        ImGui::CloseCurrentPopup();
                        InputDialog::show("Rename Workspace", "", wsName, "Rename",
                                          [&app, wsId](const std::string& newName) -> std::string {
                                              if (newName.empty())
                                                  return "Name cannot be empty.";
                                              app.renameWorkspace(wsId, newName);
                                              return "";
                                          });
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Rename");

                    ImGui::SetCursorScreenPos({actionX + actionBtnSize + actionGap, actionY});
                    if (mydear::IconButton(ICON_FA_TRASH "##del", mydear::ButtonVariant::Danger,
                                           {actionBtnSize, actionBtnSize})) {
                        const int wsId = ws.id;
                        ImGui::CloseCurrentPopup();
                        Alert::show(
                            "Delete Workspace",
                            "Are you sure you want to delete this workspace? "
                            "All connections in this workspace will be removed.",
                            {AlertButton{"Delete", [&app, wsId]() { app.deleteWorkspace(wsId); },
                                         AlertButton::Style::Destructive},
                             AlertButton{"Cancel", {}, AlertButton::Style::Cancel}});
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Delete");
                }

                ImGui::SetCursorScreenPos(nextRowPos);
                ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Selectable(newWorkspaceLabel, false, 0, {contentW, rowHeight})) {
                if (!app.canAddWorkspace()) {
                    Alert::show(
                        "Workspace Limit Reached",
                        "Free tier is limited to 2 workspaces. Activate a license to create more.");
                } else {
                    InputDialog::show("New Workspace", "", "", "Create",
                                      [&app](const std::string& name) -> std::string {
                                          if (name.empty())
                                              return "Name cannot be empty.";
                                          app.createWorkspace(name, "");
                                          return "";
                                      });
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
} // namespace

void TitleBarView::render(Application& app) {
    const auto& colors = app.getCurrentColors();
    const auto bar = mydear::BeginTitleBar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::S, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    const float rowY = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Theme::Spacing::S, rowY));

    if (barButton("###sidebar", glyph("sidebar.left", ICON_FA_BARS), "Toggle Sidebar"))
        app.setSidebarVisible(!app.isSidebarVisible());
    ImGui::SameLine();
    if (barButton("###connect", glyph("plus", ICON_FA_PLUS), "New Connection") &&
        app.getDatabaseSidebar())
        app.getDatabaseSidebar()->showConnectionDialog();
    // linux only: sparkle and winsparkle prompt by themselves
    if (mydear::updater::Available()) {
        ImGui::SameLine();
        const std::string tip = "Update available: v" + mydear::updater::LatestVersion();
        if (barButton("###update", ICON_FA_CIRCLE_ARROW_UP, tip.c_str()))
            mydear::updater::Check();
    }
    const float leftEnd = ImGui::GetItemRectMax().x;

    // right side, laid out from the end
    const std::string wsName = app.getCurrentWorkspaceName();
    const auto wanted = [](const char* id, float fallback) {
        const auto it = gWidths.find(ImGui::GetID(id));
        return it != gWidths.end() ? std::max(ImGui::GetFrameHeight(), it->second) : fallback;
    };
    const float side = ImGui::GetFrameHeight();
    const float gearW = wanted("###settings", side);
    const float wsW = wanted("###workspace", ImGui::CalcTextSize(wsName.c_str()).x + side * 2);
    const float right = bar.pos.x + bar.size.x - bar.rightInset - Theme::Spacing::L;
    const float wsX = right - gearW - Theme::Spacing::S - wsW;

    // title, centred, unless it would run into the buttons
#ifdef NDEBUG
    const char* title = APP_NAME;
#else
    const char* title = "DearSQL (Debug)";
#endif
    const float titleW = ImGui::CalcTextSize(title).x;
    const float titleX = bar.pos.x + (bar.size.x - titleW) * 0.5f;
    if (titleX > leftEnd + Theme::Spacing::M && titleX + titleW < wsX - Theme::Spacing::M)
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(titleX, bar.pos.y + (bar.size.y - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(colors.subtext1), title);

    ImGui::SetCursorScreenPos(ImVec2(wsX, rowY));
    if (barButton("###workspace", glyph("chevron.down", ICON_FA_CHEVRON_DOWN), "Workspace",
                  wsName.c_str())) {
        ImGui::OpenPopup("##WorkspacePopup");
        gPopupPos = ImVec2(wsX, bar.pos.y + bar.size.y);
    }
    renderWorkspacePopup(app);
    ImGui::SameLine();
    if (barButton("###settings", glyph("gearshape", ICON_FA_GEAR), "Settings"))
        SettingsDialog::instance().open();

    ImGui::PopStyleVar(2);
    mydear::EndTitleBar();
}
