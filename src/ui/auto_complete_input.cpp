#include "ui/auto_complete_input.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

AutoCompleteInput::AutoCompleteInput(Config config) : config(std::move(config)) {}

bool AutoCompleteInput::render(const char* label, char* buffer, const size_t bufferSize) {
    currentBuffer = buffer;
    currentBufferSize = bufferSize;

    // Apply pending auto-complete from previous frame (before input field)
    applyPendingAutoComplete();

    ImGui::SetNextItemWidth(config.width);

    // Check if Enter should be consumed before the input
    const bool shouldConsumeEnter =
        showAutoComplete &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
        selectedSuggestionIndex >= 0;

    bool enterPressed = false;

    // Set keyboard focus if requested
    if (shouldRefocusInput) {
        ImGui::SetKeyboardFocusHere();
        shouldRefocusInput = false;
    }

    // Handle input with appropriate flags
    ImGuiInputTextFlags inputFlags = config.flags;
    if (shouldConsumeEnter) {
        // Remove EnterReturnsTrue flag when auto-complete will consume Enter
        inputFlags &= ~ImGuiInputTextFlags_EnterReturnsTrue;
    }

    // Store the ID before creating the input to check focus later
    ImGuiID inputID = ImGui::GetID(label);

    enterPressed = ImGui::InputTextWithHint(label, config.hint.c_str(), buffer, bufferSize,
                                            inputFlags, inputTextCallback, this);

    // Check if this input is focused
    const bool isFocused = ImGui::IsItemActive() || ImGui::IsItemFocused();

    if (isFocused) {
        // Draw visual emphasis for focused state
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();

        // Draw a highlighted border with glow effect
        drawList->AddRect(min, max, IM_COL32(51, 153, 255, 255), 4.0f, 0, 2.0f);

        // Outer glow (more subtle)
        drawList->AddRect(ImVec2(min.x - 1, min.y - 1), ImVec2(max.x + 1, max.y + 1),
                          IM_COL32(51, 153, 255, 100), 4.0f, 0, 1.0f);

        // Outermost glow (very subtle)
        drawList->AddRect(ImVec2(min.x - 2, min.y - 2), ImVec2(max.x + 2, max.y + 2),
                          IM_COL32(51, 153, 255, 50), 4.0f, 0, 1.0f);
    }

    // Show auto-complete popup if there are suggestions
    renderAutoCompletePopup();

    // Only process Enter if not consumed by auto-complete and no pending completion
    const bool shouldProcessEnter =
        enterPressed && !autoCompleteConsumedEnter && pendingAutoComplete.empty();

    // Reset the Enter consumed flag for next frame
    if (autoCompleteConsumedEnter) {
        autoCompleteConsumedEnter = false;
    }

    // Call onSubmit callback if Enter was pressed and not consumed
    if (shouldProcessEnter && config.onSubmit) {
        config.onSubmit();
        hideAutoComplete();
    }

    return shouldProcessEnter;
}

void AutoCompleteInput::setKeywords(const std::vector<std::string>& keywords) {
    config.keywords = keywords;
}

void AutoCompleteInput::addKeywords(const std::vector<std::string>& keywords) {
    config.keywords.insert(config.keywords.end(), keywords.begin(), keywords.end());

    // Remove duplicates and sort
    std::ranges::sort(config.keywords);
    config.keywords.erase(std::ranges::unique(config.keywords).begin(), config.keywords.end());
}

void AutoCompleteInput::clearKeywords() {
    config.keywords.clear();
}

void AutoCompleteInput::hideAutoComplete() {
    showAutoComplete = false;
    autoCompleteSuggestions.clear();
    selectedSuggestionIndex = -1;
}

bool AutoCompleteInput::isAutoCompleteVisible() const {
    return showAutoComplete;
}

std::string AutoCompleteInput::getText() const {
    return currentBuffer ? std::string(currentBuffer) : "";
}

void AutoCompleteInput::setText(const std::string& text) const {
    if (currentBuffer && currentBufferSize > 0) {
        strncpy(currentBuffer, text.c_str(), currentBufferSize - 1);
        currentBuffer[currentBufferSize - 1] = '\0';
    }
}

void AutoCompleteInput::clearText() {
    if (currentBuffer && currentBufferSize > 0) {
        memset(currentBuffer, 0, currentBufferSize);
    }
    hideAutoComplete();
}

int AutoCompleteInput::inputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* input = static_cast<AutoCompleteInput*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Tab key pressed - trigger auto-completion
        input->triggerAutoComplete(data);
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        // Text changed - update suggestions
        input->updateAutoCompleteSuggestions(data);
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        // Handle cursor positioning after refocus
        if (input->needsCursorReposition) {
            data->CursorPos = data->BufTextLen;
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
            input->needsCursorReposition = false;
        }
    }

    return 0;
}

void AutoCompleteInput::updateAutoCompleteSuggestions(const ImGuiInputTextCallbackData* data) {
    std::string currentText(data->Buf, data->BufTextLen);
    autoCompleteSuggestions.clear();
    selectedSuggestionIndex = -1;

    // Find the word at cursor position
    int wordStart = data->CursorPos;
    while (wordStart > 0 && data->Buf[wordStart - 1] != ' ' && data->Buf[wordStart - 1] != '(' &&
           data->Buf[wordStart - 1] != ',' && data->Buf[wordStart - 1] != '=' &&
           data->Buf[wordStart - 1] != '<' && data->Buf[wordStart - 1] != '>' &&
           data->Buf[wordStart - 1] != '!') {
        wordStart--;
    }

    const std::string currentWord(data->Buf + wordStart, data->CursorPos - wordStart);
    if (currentWord.empty()) {
        showAutoComplete = false;
        return;
    }

    // Convert current word to lowercase for comparison
    std::string lowerWord = currentWord;
    std::ranges::transform(lowerWord, lowerWord.begin(), ::tolower);

    // Add keyword suggestions
    for (const auto& keyword : config.keywords) {
        std::string lowerKeyword = keyword;
        std::ranges::transform(lowerKeyword, lowerKeyword.begin(), ::tolower);
        if (lowerKeyword.find(lowerWord) == 0) {
            autoCompleteSuggestions.push_back(keyword);
        }
    }

    // Sort suggestions alphabetically
    std::ranges::sort(autoCompleteSuggestions);

    // Remove duplicates
    autoCompleteSuggestions.erase(std::ranges::unique(autoCompleteSuggestions).begin(),
                                  autoCompleteSuggestions.end());

    showAutoComplete = !autoCompleteSuggestions.empty();
    autoCompleteWordStart = wordStart;
    autoCompleteWordEnd = data->CursorPos;

    // Auto-select first suggestion when popup appears
    if (!autoCompleteSuggestions.empty()) {
        selectedSuggestionIndex = 0;
    }
}

void AutoCompleteInput::triggerAutoComplete(ImGuiInputTextCallbackData* data) {
    if (!showAutoComplete || autoCompleteSuggestions.empty()) {
        // No suggestions, try to generate them
        updateAutoCompleteSuggestions(data);
        if (!autoCompleteSuggestions.empty()) {
            selectedSuggestionIndex = 0;
        }
        return;
    }

    // If we have suggestions and one is selected, apply it
    if (selectedSuggestionIndex >= 0 && selectedSuggestionIndex < autoCompleteSuggestions.size()) {
        const std::string& suggestion = autoCompleteSuggestions[selectedSuggestionIndex];

        // Delete the current partial word
        data->DeleteChars(autoCompleteWordStart, autoCompleteWordEnd - autoCompleteWordStart);

        // Insert the suggestion
        data->InsertChars(autoCompleteWordStart, suggestion.c_str());

        // Hide auto-complete
        hideAutoComplete();
    } else if (!autoCompleteSuggestions.empty()) {
        // No selection, select the first one
        selectedSuggestionIndex = 0;
    }
}

void AutoCompleteInput::renderAutoCompletePopup() {
    if (!showAutoComplete || autoCompleteSuggestions.empty()) {
        return;
    }

    // Position the popup below the input field
    const ImVec2 inputPos = ImGui::GetItemRectMin();
    const ImVec2 inputSize = ImGui::GetItemRectSize();
    ImGui::SetNextWindowPos(ImVec2(inputPos.x, inputPos.y + inputSize.y));

    // Calculate popup size
    constexpr float maxWidth = 300.0f;
    const float itemHeight = ImGui::GetTextLineHeightWithSpacing();
    constexpr float verticalPadding = 8.0f;
    const float maxHeight =
        static_cast<float>(autoCompleteSuggestions.size()) * itemHeight + verticalPadding;

    ImGui::SetNextWindowSize(ImVec2(maxWidth, maxHeight));

    // Create popup window - remove scrollbar entirely
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("##AutoComplete", nullptr, flags)) {
        // Handle keyboard navigation
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (selectedSuggestionIndex > 0) {
                selectedSuggestionIndex--;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (selectedSuggestionIndex < static_cast<int>(autoCompleteSuggestions.size()) - 1) {
                selectedSuggestionIndex++;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            // Handle Enter key to apply selected suggestion
            if (selectedSuggestionIndex >= 0 &&
                selectedSuggestionIndex < autoCompleteSuggestions.size()) {
                pendingAutoComplete = autoCompleteSuggestions[selectedSuggestionIndex];
                pendingAutoCompleteStart = autoCompleteWordStart;
                pendingAutoCompleteEnd = autoCompleteWordEnd;
                hideAutoComplete();
                // Mark that Enter was consumed by auto-complete
                autoCompleteConsumedEnter = true;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            hideAutoComplete();
        }

        // Render suggestions
        for (int i = 0; i < autoCompleteSuggestions.size(); i++) {
            bool isSelected = (i == selectedSuggestionIndex);

            if (ImGui::Selectable(autoCompleteSuggestions[i].c_str(), isSelected)) {
                // Store the suggestion to apply after this frame
                pendingAutoComplete = autoCompleteSuggestions[i];
                pendingAutoCompleteStart = autoCompleteWordStart;
                pendingAutoCompleteEnd = autoCompleteWordEnd;
                hideAutoComplete();
            }

            if (isSelected) {
                ImGui::SetItemDefaultFocus();
                // Ensure selected item is visible
                ImGui::SetScrollHereY();
            }
        }
    }
    ImGui::End();
}

void AutoCompleteInput::applyPendingAutoComplete() {
    if (pendingAutoComplete.empty() || !currentBuffer) {
        return;
    }

    // Build the new text
    const std::string currentText(currentBuffer);
    const std::string newText = currentText.substr(0, pendingAutoCompleteStart) +
                                pendingAutoComplete + " " +
                                currentText.substr(pendingAutoCompleteEnd);

    // Update the buffer
    strncpy(currentBuffer, newText.c_str(), currentBufferSize - 1);
    currentBuffer[currentBufferSize - 1] = '\0';

    // Clear pending
    pendingAutoComplete.clear();
    pendingAutoCompleteStart = 0;
    pendingAutoCompleteEnd = 0;

    // Request focus for the input field and cursor repositioning
    shouldRefocusInput = true;
    needsCursorReposition = true;
}
