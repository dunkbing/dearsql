#include "ui/settings_dialog.hpp"

#include "app_state.hpp"
#include "application.hpp"
#include "imgui.h"
#include "mydear/button.hpp"
#include "mydear/menu.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "utils/cli_command.hpp"
#include "utils/master_secret.hpp"

#include <cfloat>
#include <format>

namespace {
    constexpr const char* POPUP_ID = "Settings";
} // namespace

SettingsDialog::SettingsDialog() = default;
SettingsDialog::~SettingsDialog() = default;

SettingsDialog& SettingsDialog::instance() {
    static SettingsDialog dialog;
    return dialog;
}

void SettingsDialog::open() {
    open_ = true;
    pendingOpen_ = true;
    cliInstalled_ = CliCommand::installed();
    cliStatus_.clear();
    keyStoreStatus_.clear();
}

void SettingsDialog::render() {
    if (!open_) {
        return;
    }
    if (pendingOpen_) {
        ImGui::OpenPopup(POPUP_ID);
        pendingOpen_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    center.y -= 10.0f;
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(mydear::FitScreen(ImVec2(620.0f, 0.0f)), ImGuiCond_Appearing);
    mydear::CapToScreen();

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::CornerRadius::MEDIUM);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface2);
    if (!ImGui::BeginPopupModal(POPUP_ID, &open_,
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::Spacing::M, Theme::Spacing::S));

    // --- Appearance ---
    ImGui::SeparatorText("Appearance");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Theme");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (mydear::BeginSelect("##theme", app.getTheme().name)) {
        for (const auto& theme : Theme::THEMES) {
            const bool selected = &theme == &app.getTheme();
            if (ImGui::Selectable(theme.name, selected))
                app.setTheme(theme.id);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        mydear::EndSelect();
    }

    float scale = app.getFontScale();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Font Size");
    ImGui::SameLine();
    if (mydear::Button("A-")) {
        app.setFontScale(scale - 0.1f);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d%%", static_cast<int>(scale * 100));
    ImGui::SameLine();
    if (mydear::Button("A+")) {
        app.setFontScale(scale + 0.1f);
    }

    // --- Performance ---
    ImGui::SeparatorText("Performance");
    if (bool on = app.showFps(); ImGui::Checkbox("Show frame rate", &on)) {
        app.setShowFps(on);
    }
#if !defined(__linux__) && !defined(DEARSQL_MOBILE) // gtk and uikit pace frames to the display
    ImGui::SameLine(0.0f, Theme::Spacing::XL);
    if (bool on = app.vsync(); ImGui::Checkbox("VSync", &on)) {
        app.setVSync(on);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw in step with the display's refresh rate. Off can cut input lag, "
                          "at the cost of more power.");
    }
    if (!app.vsync()) {
        constexpr int kLimits[] = {30, 60, 120, 144, 240, 0};
        auto label = [](int fps) { return fps ? std::format("{} FPS", fps) : "Unlimited"; };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Frame Rate Limit");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (mydear::BeginSelect("##max_fps", label(app.maxFps()).c_str())) {
            for (const int fps : kLimits) {
                if (ImGui::Selectable(label(fps).c_str(), fps == app.maxFps())) {
                    app.setMaxFps(fps);
                }
            }
            mydear::EndSelect();
        }
    }
#endif

    if (CliCommand::supported()) {
        ImGui::SeparatorText("Command Line");
        if (cliInstalled_) {
            ImGui::TextUnformatted(("dearsql is installed at " + CliCommand::linkPath()).c_str());
        } else if (mydear::Button("Install 'dearsql' Command")) {
            cliStatus_ = CliCommand::install();
            cliInstalled_ = cliStatus_.empty();
        }
        if (!cliStatus_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cliStatus_.c_str());
        }
        ImGui::TextDisabled(
            "dearsql --tui: terminal client   dearsql --mcp: tools for coding agents");
    }

#ifndef DEARSQL_MOBILE
    if (MasterSecret::keychainSupported()) {
        ImGui::SeparatorText("Security");
#if defined(__APPLE__)
        const char* store = "Keychain";
#elif defined(_WIN32)
        const char* store = "Credential Manager";
#else
        const char* store = "system keyring";
#endif
        bool keychain = MasterSecret::usingKeychain();
        if (ImGui::Checkbox(std::format("Keep the encryption key in the {}", store).c_str(),
                            &keychain)) {
            keyStoreStatus_ = MasterSecret::moveToKeychain(keychain, [&] {
                return app.getAppState() &&
                       app.getAppState()->setSetting("use_keychain", keychain ? "1" : "0");
            });
        }
        if (!keyStoreStatus_.empty()) {
            ImGui::TextDisabled("%s", keyStoreStatus_.c_str());
        }
        ImGui::TextDisabled(
            "Saved passwords are encrypted with a key kept in ~/.dearsql/master.key,\n"
            "readable only by you. The %s may ask before letting DearSQL read it.",
            store);
    }
#endif

#ifndef DEARSQL_MOBILE
    // --- Actions ---
    ImGui::SeparatorText("About");
    if (onManageLicense && mydear::Button("Manage License...")) {
        onManageLicense();
    }
    ImGui::SameLine();
    if (mydear::Button("Check for Updates...")) {
        checkForUpdates();
    }
    ImGui::SameLine();
    if (onReportBug && mydear::Button("Report Bug...")) {
        onReportBug();
    }
#endif

    // esc closes, unless a field, dropdown or a dialog on top has it
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleVar(2); // ItemSpacing, FramePadding
    ImGui::EndPopup();
    ImGui::PopStyleColor(); // Border
    ImGui::PopStyleVar(2);  // WindowBorderSize, WindowRounding
}
