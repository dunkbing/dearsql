#include "ui/input_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <cstring>

InputDialog& InputDialog::instance() {
    static InputDialog instance;
    return instance;
}

void InputDialog::show(const std::string& title, const std::string& label,
                       const std::string& initialValue, const std::string& confirmButtonText,
                       ConfirmCallback onConfirm, CancelCallback onCancel) {
    showWithValidation(title, label, initialValue, confirmButtonText, nullptr, std::move(onConfirm),
                       std::move(onCancel));
}

void InputDialog::showWithValidation(const std::string& title, const std::string& label,
                                     const std::string& initialValue,
                                     const std::string& confirmButtonText,
                                     std::function<std::string(const std::string&)> validator,
                                     ConfirmCallback onConfirm, CancelCallback onCancel) {
    reset();
    dialogTitle = title;
    inputLabel = label;
    originalValue = initialValue;
    confirmText = confirmButtonText;
    validationFunc = std::move(validator);
    confirmCallback = std::move(onConfirm);
    cancelCallback = std::move(onCancel);
    std::strncpy(inputBuffer, initialValue.c_str(), sizeof(inputBuffer) - 1);
    isDialogOpen = true;
}

void InputDialog::render() {
    if (!isDialogOpen)
        return;

    if (!ImGui::IsPopupOpen(dialogTitle.c_str())) {
        ImGui::OpenPopup(dialogTitle.c_str());
    }

    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal(dialogTitle.c_str(), &isDialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& theme = Application::getInstance().getCurrentColors();

        // Show original value if provided
        if (!originalValue.empty()) {
            ImGui::Text("Current: %s", originalValue.c_str());
            ImGui::Spacing();
        }

        // Input label and field
        if (!inputLabel.empty()) {
            ImGui::Text("%s", inputLabel.c_str());
        }
        ImGui::SetNextItemWidth(-1);

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        const bool enterPressed =
            ImGui::InputText("##input_field", inputBuffer, sizeof(inputBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue);

        // Validation
        std::string currentValue = inputBuffer;
        std::string validationError;
        bool isValid = true;

        if (validationFunc) {
            validationError = validationFunc(currentValue);
            isValid = validationError.empty();
        }

        // Show validation error
        if (!validationError.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.peach);
            ImGui::TextWrapped("%s", validationError.c_str());
            ImGui::PopStyleColor();
        }

        // Show error message (from external source)
        if (!errorMessage.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
            ImGui::TextWrapped("%s", errorMessage.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool canConfirm = std::strlen(inputBuffer) > 0 && isValid;

        if (!canConfirm) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button(confirmText.c_str(), ImVec2(100, 0)) || (enterPressed && canConfirm)) {
            if (confirmCallback) {
                errorMessage.clear(); // Clear any previous error
                confirmCallback(currentValue);
            }
            // Only close if no error was set by the callback
            if (errorMessage.empty()) {
                isDialogOpen = false;
            }
        }

        if (!canConfirm) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            if (cancelCallback) {
                cancelCallback();
            }
            isDialogOpen = false;
        }

        ImGui::EndPopup();
    }

    if (!isDialogOpen) {
        reset();
    }
}

void InputDialog::reset() {
    dialogTitle.clear();
    inputLabel.clear();
    originalValue.clear();
    confirmText.clear();
    std::memset(inputBuffer, 0, sizeof(inputBuffer));
    confirmCallback = nullptr;
    cancelCallback = nullptr;
    validationFunc = nullptr;
    errorMessage.clear();
}
