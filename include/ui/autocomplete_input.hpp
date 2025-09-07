#pragma once

#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

class AutoCompleteInput {
public:
    struct Config {
        float width = 400.0f;
        const char* hint = "";
        int bufferSize = 512;
        float popupMaxWidth = 300.0f;
        int maxSuggestionsShown = 10;
        bool caseSensitive = false;
    };

    AutoCompleteInput(const std::string& id, Config config = {});
    ~AutoCompleteInput() = default;

    // Set the keywords/suggestions for auto-completion
    void setSuggestions(const std::vector<std::string>& suggestions);

    // Add additional keywords (useful for combining multiple sources)
    void addSuggestions(const std::vector<std::string>& suggestions);

    // Clear all suggestions
    void clearSuggestions();

    // Get current text
    [[nodiscard]] std::string getText() const {
        return std::string(buffer);
    }

    // Set text programmatically
    void setText(const std::string& text);

    // Clear the input
    void clear();

    // Render the input and handle auto-completion
    // Returns true if Enter was pressed (and not consumed by auto-complete)
    bool render();

    // Optional callback when text changes
    void setOnChange(std::function<void(const std::string&)> callback) {
        onChange = callback;
    }

    // Optional callback when Enter is pressed
    void setOnEnter(std::function<void(const std::string&)> callback) {
        onEnter = callback;
    }

private:
    std::string id;
    Config config;
    char* buffer;
    std::vector<std::string> allSuggestions;

    // Auto-completion state
    bool showAutoComplete = false;
    std::vector<std::string> currentSuggestions;
    int selectedIndex = -1;
    int wordStart = 0;
    int wordEnd = 0;

    // Deferred actions
    std::string pendingCompletion;
    int pendingCompletionStart = 0;
    int pendingCompletionEnd = 0;
    bool shouldRefocus = false;
    bool needsCursorReposition = false;
    bool enterConsumed = false;

    // Callbacks
    std::function<void(const std::string&)> onChange;
    std::function<void(const std::string&)> onEnter;

    // Helper methods
    void updateSuggestions(ImGuiInputTextCallbackData* data);
    void triggerCompletion(ImGuiInputTextCallbackData* data);
    void renderPopup();
    void hideAutoComplete();
    void applyPendingCompletion();

    // Static callback for ImGui
    static int inputCallback(ImGuiInputTextCallbackData* data);
};
