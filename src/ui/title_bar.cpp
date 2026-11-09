#include "ui/title_bar.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "config.hpp"
#include "mydear/button.hpp"
#include "mydear/title_bar.hpp"
#include "platform/alert.hpp"
#include "themes.hpp"
#include "ui/input_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include <algorithm>
#include <string>

namespace {
    ImVec2 gPopupPos; // under the workspace button

    // a flat toolbar button: no fill until hovered
    bool barButton(const char* label, const char* tooltip, const ImVec2& size = ImVec2(0, 0)) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        const bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor();
        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }

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
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::XS, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    const float rowY = ImGui::GetCursorScreenPos().y;

    if (barButton(ICON_FA_BARS "##sidebar", "Toggle Sidebar"))
        app.setSidebarVisible(!app.isSidebarVisible());
    ImGui::SameLine();
    // fa6's plus is the ascii '+', drawn by the text font at text size: draw it instead
    const float side = ImGui::GetFrameHeight();
    if (barButton("##connect", "New Connection", ImVec2(side, side)) && app.getDatabaseSidebar())
        app.getDatabaseSidebar()->showConnectionDialog();
    {
        const ImVec2 c((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                       (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        ImGui::GetWindowDrawList()->AddLine({c.x - 5, c.y}, {c.x + 5, c.y}, col, 1.5f);
        ImGui::GetWindowDrawList()->AddLine({c.x, c.y - 5}, {c.x, c.y + 5}, col, 1.5f);
    }
    const float leftEnd = ImGui::GetItemRectMax().x;

    // right side, laid out from the end
    const std::string wsLabel = app.getCurrentWorkspaceName() + "  " ICON_FA_CHEVRON_DOWN;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gearW = ImGui::CalcTextSize(ICON_FA_GEAR).x + style.FramePadding.x * 2;
    const float wsW = ImGui::CalcTextSize(wsLabel.c_str()).x + style.FramePadding.x * 2;
    const float right = bar.pos.x + bar.size.x - bar.rightInset - Theme::Spacing::M;
    const float wsX = right - gearW - Theme::Spacing::XS - wsW;

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
    if (barButton((wsLabel + "##workspace").c_str(), "Workspace")) {
        ImGui::OpenPopup("##WorkspacePopup");
        gPopupPos = ImVec2(wsX, bar.pos.y + bar.size.y);
    }
    renderWorkspacePopup(app);
    ImGui::SameLine();
    if (barButton(ICON_FA_GEAR "##settings", "Settings"))
        SettingsDialog::instance().open();

    ImGui::PopStyleVar(2);
    mydear::EndTitleBar();
}
