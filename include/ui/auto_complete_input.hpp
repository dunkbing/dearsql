#pragma once

#include "imgui.h"
#include <functional>
#include <string>

class AutoCompleteInput {
public:
    struct Config {
        std::string hint = "Enter text...";
        float width = 400.0f;
        std::vector<std::string> keywords;
        std::function<void()> onSubmit = nullptr;
        ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackAlways;
    };

    explicit AutoCompleteInput(Config config);
    ~AutoCompleteInput() = default;

    bool render(const char* label, char* buffer, size_t bufferSize);

    void setKeywords(const std::vector<std::string>& keywords);
    void addKeywords(const std::vector<std::string>& keywords);
    void clearKeywords();

    void hideAutoComplete();
    [[nodiscard]] bool isAutoCompleteVisible() const;

    [[nodiscard]] std::string getText() const;
    void setText(const std::string& text) const;
    void clearText();

private:
    Config config;

    // Auto-complete state
    std::vector<std::string> autoCompleteSuggestions;
    bool showAutoComplete = false;
    int selectedSuggestionIndex = -1;
    int autoCompleteWordStart = 0;
    int autoCompleteWordEnd = 0;

    // Pending completion state
    std::string pendingAutoComplete;
    int pendingAutoCompleteStart = 0;
    int pendingAutoCompleteEnd = 0;
    bool shouldRefocusInput = false;
    bool needsCursorReposition = false;
    bool autoCompleteConsumedEnter = false;

    // Current text buffer reference
    char* currentBuffer = nullptr;
    size_t currentBufferSize = 0;

    // Callbacks
    static int inputTextCallback(ImGuiInputTextCallbackData* data);

    // Internal methods
    void updateAutoCompleteSuggestions(const ImGuiInputTextCallbackData* data);
    void triggerAutoComplete(ImGuiInputTextCallbackData* data);
    void renderAutoCompletePopup();
    void applyPendingAutoComplete();
};
