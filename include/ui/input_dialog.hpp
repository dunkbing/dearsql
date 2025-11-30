#pragma once

#include <functional>
#include <string>

class InputDialog {
public:
    using ConfirmCallback = std::function<void(const std::string&)>;
    using CancelCallback = std::function<void()>;

    // Singleton access
    static InputDialog& instance();

    // Delete copy/move
    InputDialog(const InputDialog&) = delete;
    InputDialog& operator=(const InputDialog&) = delete;
    InputDialog(InputDialog&&) = delete;
    InputDialog& operator=(InputDialog&&) = delete;

    /**
     * @brief Show an input dialog
     *
     * @param title Dialog title (e.g., "Rename Table")
     * @param label Label for the input field (e.g., "New name:")
     * @param initialValue Initial value for the input field
     * @param confirmButtonText Text for the confirm button (e.g., "Rename")
     * @param onConfirm Callback with the entered string when user confirms
     * @param onCancel Optional callback when user cancels
     */
    void show(const std::string& title, const std::string& label, const std::string& initialValue,
              const std::string& confirmButtonText, ConfirmCallback onConfirm,
              CancelCallback onCancel = nullptr);

    /**
     * @brief Show an input dialog with validation
     *
     * @param title Dialog title
     * @param label Label for the input field
     * @param initialValue Initial value for the input field
     * @param confirmButtonText Text for the confirm button
     * @param validator Function that returns empty string if valid, or error message if invalid
     * @param onConfirm Callback with the entered string when user confirms
     * @param onCancel Optional callback when user cancels
     */
    void showWithValidation(const std::string& title, const std::string& label,
                            const std::string& initialValue, const std::string& confirmButtonText,
                            std::function<std::string(const std::string&)> validator,
                            ConfirmCallback onConfirm, CancelCallback onCancel = nullptr);

    void render();

    bool isOpen() const {
        return isDialogOpen;
    }

    void setError(const std::string& error) {
        errorMessage = error;
    }

    void clearError() {
        errorMessage.clear();
    }

private:
    InputDialog() = default;
    ~InputDialog() = default;

    bool isDialogOpen = false;

    std::string dialogTitle;
    std::string inputLabel;
    std::string originalValue;
    std::string confirmText;
    char inputBuffer[256] = {0};

    ConfirmCallback confirmCallback;
    CancelCallback cancelCallback;
    std::function<std::string(const std::string&)> validationFunc;

    std::string errorMessage;

    void reset();
};
