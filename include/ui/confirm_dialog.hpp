#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * @brief A reusable confirmation dialog component (singleton)
 *
 * Use this for any destructive action that requires user confirmation.
 */
class ConfirmDialog {
public:
    using Callback = std::function<void()>;

    // Singleton access
    static ConfirmDialog& instance();

    // Delete copy/move
    ConfirmDialog(const ConfirmDialog&) = delete;
    ConfirmDialog& operator=(const ConfirmDialog&) = delete;
    ConfirmDialog(ConfirmDialog&&) = delete;
    ConfirmDialog& operator=(ConfirmDialog&&) = delete;

    /**
     * @brief Show a confirmation dialog
     *
     * @param title Dialog title (e.g., "Delete Table")
     * @param message Main message to display
     * @param details List of bullet points explaining the consequences
     * @param confirmButtonText Text for the confirm button (e.g., "Delete")
     * @param onConfirm Callback when user confirms
     * @param onCancel Optional callback when user cancels
     */
    void show(const std::string& title, const std::string& message,
              const std::vector<std::string>& details, const std::string& confirmButtonText,
              Callback onConfirm, Callback onCancel = nullptr);

    // Render the dialog (call from UI loop)
    void render();

    // Check if dialog is open
    bool isOpen() const {
        return isDialogOpen;
    }

    // Set an error message to display in the dialog
    void setError(const std::string& error) {
        errorMessage = error;
    }

private:
    ConfirmDialog() = default;
    ~ConfirmDialog() = default;

    bool isDialogOpen = false;

    std::string dialogTitle;
    std::string dialogMessage;
    std::vector<std::string> dialogDetails;
    std::string confirmText;

    Callback confirmCallback;
    Callback cancelCallback;

    std::string errorMessage;

    void reset();
};
