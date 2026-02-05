#include "ui/confirm_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"

ConfirmDialog& ConfirmDialog::instance() {
    static ConfirmDialog instance;
    return instance;
}

void ConfirmDialog::show(const std::string& title, const std::string& message,
                         const std::vector<std::string>& details,
                         const std::string& confirmButtonText, Callback onConfirm,
                         Callback onCancel) {
    dialogTitle = title;
    dialogMessage = message;
    dialogDetails = details;
    confirmText = confirmButtonText;
    confirmCallback = std::move(onConfirm);
    cancelCallback = std::move(onCancel);
    errorMessage.clear();
    isDialogOpen = true;
}

void ConfirmDialog::render() {
    if (!isDialogOpen)
        return;

    if (!ImGui::IsPopupOpen(dialogTitle.c_str())) {
        ImGui::OpenPopup(dialogTitle.c_str());
    }

    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);

    // Square corners
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

    if (ImGui::BeginPopupModal(dialogTitle.c_str(), &isDialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& theme = Application::getInstance().getCurrentColors();

        // Warning header
        ImGui::PushStyleColor(ImGuiCol_Text, theme.peach);
        ImGui::Text("Warning: This action cannot be undone!");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Main message
        ImGui::TextWrapped("%s", dialogMessage.c_str());
        ImGui::Spacing();

        // Details (bullet points)
        if (!dialogDetails.empty()) {
            ImGui::Text("This will:");
            for (const auto& detail : dialogDetails) {
                ImGui::BulletText("%s", detail.c_str());
            }
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Error message if any
        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
            ImGui::TextWrapped("Error: %s", errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        // Buttons
        ImGui::PushStyleColor(ImGuiCol_Button, theme.red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.maroon);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(theme.red.x * 0.8f, theme.red.y * 0.8f,
                                                            theme.red.z * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.crust);

        if (ImGui::Button(confirmText.c_str(), ImVec2(120, 0))) {
            if (confirmCallback) {
                confirmCallback();
            }
            isDialogOpen = false;
        }

        ImGui::PopStyleColor(4);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            if (cancelCallback) {
                cancelCallback();
            }
            isDialogOpen = false;
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3); // WindowRounding, PopupRounding, FrameRounding

    if (!isDialogOpen) {
        reset();
    }
}

void ConfirmDialog::reset() {
    dialogTitle.clear();
    dialogMessage.clear();
    dialogDetails.clear();
    confirmText.clear();
    confirmCallback = nullptr;
    cancelCallback = nullptr;
    errorMessage.clear();
}
