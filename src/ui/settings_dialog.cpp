#include "ui/settings_dialog.hpp"

#include "app_state.hpp"
#include "application.hpp"
#include "config.hpp"
#include "imgui.h"
#include "license/license_manager.hpp"
#include "mydear/button.hpp"
#include "mydear/menu.hpp"
#include "mydear/updater.hpp"
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
    if (mydear::Button("Manage License...")) {
        licenseOpen_ = true;
        licenseStatus_.clear();
        ImGui::OpenPopup("Manage License");
    }
    ImGui::SameLine();
    if (mydear::Button("Check for Updates...")) {
        mydear::updater::Check();
    }
    ImGui::SameLine();
    if (mydear::Button("Report Bug...")) {
        ImGui::GetPlatformIO().Platform_OpenInShellFn(
            ImGui::GetCurrentContext(),
            "https://github.com/dunkbing/dearsql/issues/new?labels=bug&title=%5BBug%5D");
    }
#endif

    // esc closes, unless a field, dropdown or a dialog on top has it
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !licenseOpen_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }
    renderLicense(); // after: its esc must not reach the settings

    ImGui::PopStyleVar(2); // ItemSpacing, FramePadding
    ImGui::EndPopup();
    ImGui::PopStyleColor(); // Border
    ImGui::PopStyleVar(2);  // WindowBorderSize, WindowRounding
}

void SettingsDialog::renderLicense() {
    auto& manager = LicenseManager::instance();
    const auto& colors = Application::getInstance().getCurrentColors();

    // the worker's answer, picked up on the ui thread
    if (const auto result = licenseResult_) { // a copy: reset below must not free the locked mutex
        std::lock_guard lock(result->mutex);
        if (result->done) {
            licenseError_ = !result->error.empty();
            licenseStatus_ = result->error;
            if (!licenseError_)
                licenseKey_[0] = '\0';
            licenseResult_.reset();
        }
    }
    // a request from the button: the callback only fills a shared result
    auto start = [this](bool activating, auto&& request) {
        auto result = std::make_shared<LicenseResult>();
        licenseResult_ = result;
        licenseError_ = false;
        request([result, activating](const LicenseInfo& info) {
            std::lock_guard lock(result->mutex);
            result->error = info.error;
            if (activating && !info.valid && result->error.empty())
                result->error = "Activation failed";
            result->done = true;
        });
    };

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(mydear::FitScreen(ImVec2(460.0f, 0.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Manage License", &licenseOpen_,
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize))
        return;

    const bool busy = manager.isActivating() || licenseResult_;
    if (manager.hasValidLicense()) {
        const auto info = manager.getLicenseInfo();
        std::string key = info.licenseKey;
        if (key.size() > 8)
            key = key.substr(0, 4) + "..." + key.substr(key.size() - 4);
        ImGui::TextColored(colors.green, "License active");
        ImGui::TextDisabled("Email");
        ImGui::SameLine(110);
        ImGui::TextUnformatted(info.customerEmail.empty() ? "N/A" : info.customerEmail.c_str());
        ImGui::TextDisabled("Key");
        ImGui::SameLine(110);
        ImGui::TextUnformatted(key.c_str());
        ImGui::TextDisabled("Device ID");
        ImGui::SameLine(110);
        ImGui::TextUnformatted(manager.getInstanceId().c_str());
        ImGui::BeginDisabled(busy);
        if (mydear::Button(busy ? "Deactivating..." : "Deactivate", mydear::ButtonVariant::Danger))
            start(false, [&](auto done) { manager.deactivateLicense(done); });
        ImGui::EndDisabled();
    } else {
        ImGui::TextUnformatted("Enter your license key to activate DearSQL:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter =
            ImGui::InputTextWithHint("##license_key", "XXXX-XXXX-XXXX-XXXX", licenseKey_,
                                     sizeof(licenseKey_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Device ID: %s", manager.getInstanceId().c_str());
        ImGui::TextDisabled("Don't have a license?");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("Purchase one", PURCHASE_URL);
        ImGui::BeginDisabled(busy);
        if ((mydear::Button(busy ? "Activating..." : "Activate", mydear::ButtonVariant::Primary) ||
             enter) &&
            !busy) {
            if (licenseKey_[0] == '\0') {
                licenseError_ = true;
                licenseStatus_ = "Please enter a license key";
            } else {
                const std::string key = licenseKey_;
                start(true, [&](auto done) { manager.activateLicense(key, done); });
            }
        }
        ImGui::EndDisabled();
    }
    if (!licenseStatus_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(licenseError_ ? colors.red : colors.green, "%s", licenseStatus_.c_str());
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive()) {
        licenseOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
