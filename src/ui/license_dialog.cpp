#include "ui/license_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "license/license_manager.hpp"
#include "themes.hpp"

LicenseDialog& LicenseDialog::instance() {
    static LicenseDialog inst;
    return inst;
}

void LicenseDialog::show() {
    reset();
    isDialogOpen = true;
}

void LicenseDialog::reset() {
    std::memset(licenseKeyBuffer, 0, sizeof(licenseKeyBuffer));
    errorMessage.clear();
    successMessage.clear();
    isActivating = false;
}

void LicenseDialog::render() {
    if (!isDialogOpen)
        return;

    if (!ImGui::IsPopupOpen("License###LicenseDialog")) {
        ImGui::OpenPopup("License###LicenseDialog");
    }

    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal("License###LicenseDialog", &isDialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        auto& licenseManager = LicenseManager::instance();

        if (licenseManager.hasValidLicense()) {
            renderLicensed();
        } else {
            renderUnlicensed();
        }

        ImGui::EndPopup();
    }

    if (!isDialogOpen) {
        reset();
    }
}

void LicenseDialog::renderLicensed() {
    const auto& theme = Application::getInstance().getCurrentColors();
    auto& licenseManager = LicenseManager::instance();
    const auto& info = licenseManager.getLicenseInfo();

    ImGui::PushStyleColor(ImGuiCol_Text, theme.green);
    ImGui::Text("License Active");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!info.customerEmail.empty()) {
        ImGui::Text("Email: %s", info.customerEmail.c_str());
    }

    // Mask the license key for display
    std::string maskedKey = info.licenseKey;
    if (maskedKey.length() > 8) {
        maskedKey = maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
    }
    ImGui::Text("Key: %s", maskedKey.c_str());

    if (!info.activatedAt.empty()) {
        ImGui::Text("Activated: %s", info.activatedAt.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Show error/success messages
    if (!errorMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
        ImGui::TextWrapped("%s", errorMessage.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (!successMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.green);
        ImGui::TextWrapped("%s", successMessage.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (isActivating || licenseManager.isActivating()) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Deactivate License", ImVec2(-1, 0))) {
        isActivating = true;
        errorMessage.clear();
        successMessage.clear();

        licenseManager.deactivateLicense([this](const LicenseInfo& result) {
            isActivating = false;
            if (!result.error.empty()) {
                errorMessage = result.error;
            } else {
                successMessage = "License deactivated";
            }
        });
    }

    if (isActivating || licenseManager.isActivating()) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Processing...");
    }

    ImGui::Spacing();

    if (ImGui::Button("Close", ImVec2(-1, 0))) {
        isDialogOpen = false;
    }
}

void LicenseDialog::renderUnlicensed() {
    const auto& theme = Application::getInstance().getCurrentColors();
    auto& licenseManager = LicenseManager::instance();

    ImGui::Text("Enter your license key to activate DearSQL.");
    ImGui::Spacing();

    ImGui::Text("License Key:");
    ImGui::SetNextItemWidth(-1);

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }

    bool enterPressed =
        ImGui::InputText("##license_key", licenseKeyBuffer, sizeof(licenseKeyBuffer),
                         ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();

    // Show error/success messages
    if (!errorMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
        ImGui::TextWrapped("%s", errorMessage.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (!successMessage.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.green);
        ImGui::TextWrapped("%s", successMessage.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();

    bool hasKey = std::strlen(licenseKeyBuffer) > 0;
    bool canActivate = hasKey && !isActivating && !licenseManager.isActivating();

    if (!canActivate) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Activate", ImVec2(120, 0)) || (enterPressed && canActivate)) {
        isActivating = true;
        errorMessage.clear();
        successMessage.clear();

        std::string key = licenseKeyBuffer;
        licenseManager.activateLicense(key, [this](const LicenseInfo& result) {
            isActivating = false;
            if (result.valid) {
                successMessage = "License activated successfully!";
                std::memset(licenseKeyBuffer, 0, sizeof(licenseKeyBuffer));
            } else {
                errorMessage = result.error.empty() ? "Activation failed" : result.error;
            }
        });
    }

    if (!canActivate) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        isDialogOpen = false;
    }

    if (isActivating || licenseManager.isActivating()) {
        ImGui::SameLine();
        ImGui::Text("Activating...");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, theme.subtext0);
    ImGui::TextWrapped("Don't have a license? Purchase one at dearsql.com");
    ImGui::PopStyleColor();
}
