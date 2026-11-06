#include "ui/settings_dialog.hpp"

#include "app_state.hpp"
#include "application.hpp"
#include "imgui.h"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "utils/button.hpp"
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
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);

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
    bool dark = app.isDarkTheme();
    if (ImGui::RadioButton("Light", !dark)) {
        app.setDarkTheme(false);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Dark", dark)) {
        app.setDarkTheme(true);
    }

    float scale = app.getFontScale();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Font Size");
    ImGui::SameLine();
    if (UIUtils::Button("A-")) {
        app.setFontScale(scale - 0.1f);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d%%", static_cast<int>(scale * 100));
    ImGui::SameLine();
    if (UIUtils::Button("A+")) {
        app.setFontScale(scale + 0.1f);
    }

    if (CliCommand::supported()) {
        ImGui::SeparatorText("Command Line");
        if (cliInstalled_) {
            ImGui::TextUnformatted(("dearsql is installed at " + CliCommand::linkPath()).c_str());
        } else if (UIUtils::Button("Install 'dearsql' Command")) {
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
            keyStoreStatus_ = MasterSecret::moveToKeychain(keychain);
            if (keyStoreStatus_.empty() && app.getAppState()) {
                app.getAppState()->setSetting("use_keychain", keychain ? "1" : "0");
            }
        }
        if (!keyStoreStatus_.empty()) {
            ImGui::TextDisabled("%s", keyStoreStatus_.c_str());
        }
        ImGui::TextDisabled(
            "Saved passwords are encrypted with a key kept in ~/.dearsql/master.key,\n"
            "readable only by you. The %s may ask before letting DearSQL read it.",
            store);
    }

    // --- Actions ---
    ImGui::SeparatorText("About");
    if (onManageLicense && UIUtils::Button("Manage License...")) {
        onManageLicense();
    }
    ImGui::SameLine();
    if (UIUtils::Button("Check for Updates...")) {
        checkForUpdates();
    }
    ImGui::SameLine();
    if (onReportBug && UIUtils::Button("Report Bug...")) {
        onReportBug();
    }

    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
    if (UIUtils::Button("Close", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleVar(2); // ItemSpacing, FramePadding
    ImGui::EndPopup();
    ImGui::PopStyleColor(); // Border
    ImGui::PopStyleVar(2);  // WindowBorderSize, WindowRounding
}
