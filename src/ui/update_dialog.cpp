#include "ui/update_dialog.hpp"
#include "application.hpp"
#include "config.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"

UpdateDialog& UpdateDialog::instance() {
    static UpdateDialog inst;
    return inst;
}

void UpdateDialog::showChecking() {
    state_ = State::Checking;
    isOpen_ = true;
}

void UpdateDialog::showUpdateAvailable(const std::string& currentVersion,
                                       const std::string& newVersion,
                                       const std::string& releaseNotes) {
    currentVersion_ = currentVersion;
    newVersion_ = newVersion;
    releaseNotes_ = releaseNotes;
    state_ = State::UpdateAvailable;
    isOpen_ = true;
}

void UpdateDialog::showUpToDate() {
    state_ = State::UpToDate;
    isOpen_ = true;
}

void UpdateDialog::showDownloading() {
    state_ = State::Downloading;
    isOpen_ = true;
}

void UpdateDialog::showDownloadComplete() {
    state_ = State::DownloadComplete;
    isOpen_ = true;
}

void UpdateDialog::showError(const std::string& message) {
    errorMessage_ = message;
    state_ = State::Error;
    isOpen_ = true;
}

void UpdateDialog::render() {
    if (!isOpen_)
        return;

    if (!ImGui::IsPopupOpen("Update###UpdateDialog")) {
        ImGui::OpenPopup("Update###UpdateDialog");
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(FLT_MAX, FLT_MAX));

    const auto& theme = Application::getInstance().getCurrentColors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::CornerRadius::MEDIUM);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.surface2);
    if (ImGui::BeginPopupModal("Update###UpdateDialog", &isOpen_,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        switch (state_) {
        case State::Checking: {
            ImGui::Text("Checking for updates...");
            break;
        }

        case State::UpdateAvailable: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.green);
            ImGui::Text("A new version is available!");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Current: %s", currentVersion_.c_str());
            ImGui::Text("Latest:  %s", newVersion_.c_str());

            if (!releaseNotes_.empty()) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("%s", releaseNotes_.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (UIUtils::Button("Download Update", UIUtils::ButtonVariant::Primary,
                                ImVec2(200, 0))) {
                wantsDownload_ = true;
            }
            ImGui::SameLine();
            if (UIUtils::Button("Later", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
                isOpen_ = false;
            }
            break;
        }

        case State::UpToDate: {
            ImGui::Text("You're running the latest version (%s).", APP_VERSION);
            ImGui::Spacing();
            if (UIUtils::Button("Close", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
                isOpen_ = false;
            }
            break;
        }

        case State::Downloading: {
            ImGui::Text("Downloading update...");
            break;
        }

        case State::DownloadComplete: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.green);
            ImGui::Text("Update downloaded successfully!");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Text("Restart to apply the update.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (UIUtils::Button("Restart Now", UIUtils::ButtonVariant::Primary, ImVec2(200, 0))) {
                wantsRestart_ = true;
            }
            ImGui::SameLine();
            if (UIUtils::Button("Later", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
                isOpen_ = false;
            }
            break;
        }

        case State::Error: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
            ImGui::TextWrapped("%s", errorMessage_.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (UIUtils::Button("Close", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
                isOpen_ = false;
            }
            break;
        }

        case State::Hidden:
            break;
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (!isOpen_) {
        state_ = State::Hidden;
        wantsDownload_ = false;
        wantsRestart_ = false;
    }
}
