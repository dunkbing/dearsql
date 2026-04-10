#include "ui/auto_complete_input.hpp"
#include "application.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

AutoCompleteInput::AutoCompleteInput(Config config) : config(std::move(config)) {}

bool AutoCompleteInput::render(const char* label, char* buffer, const size_t bufferSize) {
    const auto& colors = Application::getInstance().getCurrentColors();
    currentBuffer = buffer;
    currentBufferSize = bufferSize;
    const std::string previousText = getText();

    const bool hasEndIcon = !config.endIcon.empty();
    const float iconAreaW = hasEndIcon ? ImGui::GetFrameHeight() : 0.0f;
    ImGui::SetNextItemWidth(config.width - iconAreaW);

    const bool shouldConsumeEnter =
        showAutoComplete &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
        selectedSuggestionIndex >= 0;

    bool enterPressed = false;

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

    enterPressed = ImGui::InputTextWithHint(label, config.hint.c_str(), buffer, bufferSize,
                                            inputFlags, inputTextCallback, this);

    if (ImGui::IsItemActivated()) {
        if (suppressNextActivationRefresh) {
            suppressNextActivationRefresh = false;
        } else {
            updateSuggestions(buffer, static_cast<int>(std::strlen(buffer)),
                              static_cast<int>(std::strlen(buffer)));
        }
    } else if (suppressNextActivationRefresh && !ImGui::IsItemActive()) {
        suppressNextActivationRefresh = false;
    }

    const bool showFocusedVisual = ImGui::IsItemActive() || showAutoComplete ||
                                   !pendingAutoComplete.empty() || shouldRefocusInput;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 inputMin = ImGui::GetItemRectMin();
    const ImVec2 inputMax = ImGui::GetItemRectMax();

    if (showFocusedVisual) {
        // Draw subtle visual emphasis for focused state
        const ImU32 focusColor =
            ImGui::GetColorU32(ImVec4(colors.blue.x, colors.blue.y, colors.blue.z, 0.3f));
        const ImVec2 fullMax = hasEndIcon ? ImVec2(inputMax.x + iconAreaW, inputMax.y) : inputMax;
        drawList->AddRect(inputMin, fullMax, focusColor, 3.0f, 0, 1.5f);
    }

    // Draw icon area fused to the right edge of the input
    bool endIconClicked = false;
    if (hasEndIcon) {
        const ImVec2 iconMin(inputMax.x, inputMin.y);
        const ImVec2 iconMax(inputMax.x + iconAreaW, inputMax.y);

        // background matching the input
        const bool iconHovered = ImGui::IsMouseHoveringRect(iconMin, iconMax);
        const ImU32 bgColor =
            ImGui::GetColorU32(iconHovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
        drawList->AddRectFilled(iconMin, iconMax, bgColor, 0.0f);

        // right + top + bottom border segments to complete the outline
        const ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Border);
        drawList->AddLine(iconMin, ImVec2(iconMax.x, iconMin.y), borderColor); // top
        drawList->AddLine(ImVec2(iconMax.x, iconMin.y), iconMax, borderColor); // right
        drawList->AddLine(ImVec2(iconMin.x, iconMax.y), iconMax, borderColor); // bottom

        // icon text centered
        const ImVec2 textSize = ImGui::CalcTextSize(config.endIcon.c_str());
        const ImVec2 textPos(iconMin.x + (iconAreaW - textSize.x) * 0.5f,
                             iconMin.y + (iconMax.y - iconMin.y - textSize.y) * 0.5f);
        const ImU32 iconColor =
            ImGui::GetColorU32(iconHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        drawList->AddText(textPos, iconColor, config.endIcon.c_str());

        // click detection via invisible button
        ImGui::SetCursorScreenPos(iconMin);
        if (ImGui::InvisibleButton("##endIcon", ImVec2(iconAreaW, iconMax.y - iconMin.y))) {
            endIconClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Apply filters");
        }
    }

    // Anchor the popup to the input rect (not the end-icon rect, which
    // would otherwise be the "last item" by the time the popup renders).
    const ImVec2 anchorMax = hasEndIcon ? ImVec2(inputMax.x + iconAreaW, inputMax.y) : inputMax;
    renderAutoCompletePopup(inputMin, ImVec2(anchorMax.x - inputMin.x, inputMax.y - inputMin.y));

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
        shouldRefocusInput = true;
    }

    if (endIconClicked && config.onEndIconClick) {
        config.onEndIconClick();
    }

    if (config.onChange && getText() != previousText) {
        config.onChange();
    }

    return shouldProcessEnter || endIconClicked;
}

void AutoCompleteInput::setKeywords(const std::vector<std::string>& keywords) {
    config.keywords = keywords;
}

void AutoCompleteInput::addKeywords(const std::vector<std::string>& keywords) {
    config.keywords.insert(config.keywords.end(), keywords.begin(), keywords.end());

    // Remove duplicates and sort
    std::ranges::sort(config.keywords);
    auto ret = std::ranges::unique(config.keywords);
    config.keywords.erase(ret.begin(), ret.end());
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
        if (!input->pendingAutoComplete.empty()) {
            input->applyPendingAutoComplete(data);
        }

        // Handle cursor positioning after refocus
        if (input->needsCursorReposition) {
            data->CursorPos = data->BufTextLen;
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
            input->needsCursorReposition = false;
        }
    }

    return 0;
}

void AutoCompleteInput::updateSuggestions(const char* buffer, const int textLength,
                                          const int cursorPos) {
    autoCompleteSuggestions.clear();
    selectedSuggestionIndex = -1;

    int matchStart = 0;
    int matchEnd = textLength;
    std::string currentMatchText;

    if (config.completionMode == CompletionMode::CurrentToken) {
        matchStart = cursorPos;
        while (matchStart > 0 && buffer[matchStart - 1] != ' ' && buffer[matchStart - 1] != '(' &&
               buffer[matchStart - 1] != ',' && buffer[matchStart - 1] != '=' &&
               buffer[matchStart - 1] != '<' && buffer[matchStart - 1] != '>' &&
               buffer[matchStart - 1] != '!') {
            matchStart--;
        }
        matchEnd = cursorPos;
        currentMatchText.assign(buffer + matchStart, cursorPos - matchStart);
    } else {
        currentMatchText.assign(buffer, textLength);
    }

    if (currentMatchText.empty() && !config.showSuggestionsOnEmpty) {
        showAutoComplete = false;
        return;
    }

    auto normalize = [this](std::string value) {
        if (!config.caseSensitive) {
            std::ranges::transform(value, value.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        }
        return value;
    };

    const std::string normalizedMatchText = normalize(currentMatchText);

    for (const auto& keyword : config.keywords) {
        const std::string normalizedKeyword = normalize(keyword);
        const bool matches =
            normalizedMatchText.empty() ||
            (config.matchMode == MatchMode::Prefix
                 ? normalizedKeyword.find(normalizedMatchText) == 0
                 : normalizedKeyword.find(normalizedMatchText) != std::string::npos);
        if (matches) {
            autoCompleteSuggestions.push_back(keyword);
        }
    }

    std::ranges::sort(autoCompleteSuggestions);
    auto ret = std::ranges::unique(autoCompleteSuggestions);
    autoCompleteSuggestions.erase(ret.begin(), ret.end());

    showAutoComplete = !autoCompleteSuggestions.empty();
    autoCompleteWordStart = matchStart;
    autoCompleteWordEnd = matchEnd;

    if (!autoCompleteSuggestions.empty()) {
        selectedSuggestionIndex = 0;
    }
}

void AutoCompleteInput::updateAutoCompleteSuggestions(const ImGuiInputTextCallbackData* data) {
    updateSuggestions(data->Buf, data->BufTextLen, data->CursorPos);
}

void AutoCompleteInput::commitSuggestion(const std::string& suggestion) {
    if (config.completionMode == CompletionMode::WholeInput) {
        setText(suggestion);
        hideAutoComplete();
        shouldRefocusInput = config.refocusOnComplete;
        suppressNextActivationRefresh = config.refocusOnComplete;
        return;
    }

    pendingAutoComplete = suggestion;
    pendingAutoCompleteStart = autoCompleteWordStart;
    pendingAutoCompleteEnd = autoCompleteWordEnd;
    hideAutoComplete();
    shouldRefocusInput = config.refocusOnComplete;
    suppressNextActivationRefresh = config.refocusOnComplete;
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
        commitSuggestion(autoCompleteSuggestions[selectedSuggestionIndex]);
    } else if (!autoCompleteSuggestions.empty()) {
        // No selection, select the first one
        selectedSuggestionIndex = 0;
    }
}

void AutoCompleteInput::renderAutoCompletePopup(const ImVec2& anchorPos, const ImVec2& anchorSize) {
    if (!showAutoComplete || autoCompleteSuggestions.empty()) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(anchorPos.x, anchorPos.y + anchorSize.y));

    const int visibleSuggestionCount =
        config.maxSuggestionsShown > 0
            ? std::min(static_cast<int>(autoCompleteSuggestions.size()), config.maxSuggestionsShown)
            : static_cast<int>(autoCompleteSuggestions.size());
    const float itemHeight = ImGui::GetTextLineHeightWithSpacing();
    constexpr float verticalPadding = 8.0f;
    const float maxHeight =
        static_cast<float>(visibleSuggestionCount) * itemHeight + verticalPadding;

    ImGui::SetNextWindowSize(ImVec2(anchorSize.x, maxHeight));

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

    bool popupHovered = false;
    if (ImGui::Begin("##AutoComplete", nullptr, flags)) {
        popupHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
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
                commitSuggestion(autoCompleteSuggestions[selectedSuggestionIndex]);
                // Mark that Enter was consumed by auto-complete
                autoCompleteConsumedEnter = true;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            hideAutoComplete();
        }

        // Render suggestions
        for (int i = 0; i < autoCompleteSuggestions.size(); i++) {
            const bool isSelected = (i == selectedSuggestionIndex);

            if (ImGui::Selectable(autoCompleteSuggestions[i].c_str(), isSelected)) {
                commitSuggestion(autoCompleteSuggestions[i]);
            }

            if (isSelected) {
                ImGui::SetItemDefaultFocus();
                // Ensure selected item is visible
                ImGui::SetScrollHereY();
            }
        }
    }
    ImGui::End();

    const bool inputHovered = ImGui::IsMouseHoveringRect(
        anchorPos, ImVec2(anchorPos.x + anchorSize.x, anchorPos.y + anchorSize.y));
    const bool clickedOutside = (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                                 ImGui::IsMouseClicked(ImGuiMouseButton_Right)) &&
                                !inputHovered && !popupHovered;
    if (clickedOutside) {
        hideAutoComplete();
    }
}

void AutoCompleteInput::applyPendingAutoComplete(ImGuiInputTextCallbackData* data) {
    if (pendingAutoComplete.empty() || !data) {
        return;
    }

    data->DeleteChars(pendingAutoCompleteStart, pendingAutoCompleteEnd - pendingAutoCompleteStart);

    const std::string completedText =
        config.appendSpaceOnComplete ? pendingAutoComplete + " " : pendingAutoComplete;
    data->InsertChars(pendingAutoCompleteStart, completedText.c_str());

    data->CursorPos = pendingAutoCompleteStart + static_cast<int>(completedText.size());
    data->SelectionStart = data->SelectionEnd = data->CursorPos;

    // Clear pending
    pendingAutoComplete.clear();
    pendingAutoCompleteStart = 0;
    pendingAutoCompleteEnd = 0;

    needsCursorReposition = false;
}
