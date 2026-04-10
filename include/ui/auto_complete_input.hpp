#pragma once

#include "imgui.h"
#include <functional>
#include <string>
#include <vector>

class AutoCompleteInput {
public:
    enum class MatchMode { Prefix, Substring };
    enum class CompletionMode { CurrentToken, WholeInput };

    struct Config {
        std::string hint = "Enter text...";
        float width = 400.0f;
        std::vector<std::string> keywords;
        MatchMode matchMode = MatchMode::Prefix;
        CompletionMode completionMode = CompletionMode::CurrentToken;
        bool appendSpaceOnComplete = true;
        bool showSuggestionsOnEmpty = false;
        bool caseSensitive = false;
        int maxSuggestionsShown = 10;
        bool refocusOnComplete = true;
        std::function<void()> onChange = nullptr;
        std::function<void()> onSubmit = nullptr;
        ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackAlways;
        std::string endIcon;                  // optional icon drawn inside the right edge
        std::function<void()> onEndIconClick; // called when the end icon is clicked
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
    bool suppressNextActivationRefresh = false;

    // Current text buffer reference
    char* currentBuffer = nullptr;
    size_t currentBufferSize = 0;

    // Callbacks
    static int inputTextCallback(ImGuiInputTextCallbackData* data);

    // Internal methods
    void updateSuggestions(const char* buffer, int textLength, int cursorPos);
    void updateAutoCompleteSuggestions(const ImGuiInputTextCallbackData* data);
    void commitSuggestion(const std::string& suggestion);
    void triggerAutoComplete(ImGuiInputTextCallbackData* data);
    void renderAutoCompletePopup(const ImVec2& anchorPos, const ImVec2& anchorSize);
    void applyPendingAutoComplete(ImGuiInputTextCallbackData* data);
};
