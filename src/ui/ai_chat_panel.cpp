#include "ui/ai_chat_panel.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "utils/spinner.hpp"
#include <format>

namespace {
    constexpr const char* LABEL_STOP = "Stop";
    constexpr const char* LABEL_CLEAR_CHAT = "Clear";
    constexpr const char* LABEL_SETTINGS = "Settings";
    constexpr const char* LABEL_COPY = "Copy";
    constexpr const char* LABEL_INSERT = "Insert";
    constexpr const char* LABEL_EMPTY_CHAT = "Ask a question about your database or request SQL.";

    struct ModelOption {
        const char* label;
        const char* model;
        AIProvider provider;
    };

    constexpr ModelOption MODEL_OPTIONS[] = {
        {"claude-sonnet-4-5", "claude-sonnet-4-5", AIProvider::ANTHROPIC},
        {"claude-haiku-3-5", "claude-3-5-haiku-latest", AIProvider::ANTHROPIC},
        {"gemini-2.0-flash", "gemini-2.0-flash", AIProvider::GEMINI},
        {"gemini-2.5-pro", "gemini-2.5-pro", AIProvider::GEMINI},
    };
    constexpr int MODEL_COUNT = sizeof(MODEL_OPTIONS) / sizeof(MODEL_OPTIONS[0]);

    const char* getProviderApiKeySetting(AIProvider provider) {
        return provider == AIProvider::GEMINI ? "ai_api_key_gemini" : "ai_api_key_anthropic";
    }

    float getAIInputContainerHeight() {
        // Match renderInputArea() layout:
        // top/bottom child padding + input row + row spacing + controls row
        const ImGuiStyle& style = ImGui::GetStyle();
        float inputRowHeight = ImGui::GetTextLineHeight() + Theme::Spacing::M * 2.0f;
        float controlsRowHeight = ImGui::GetFrameHeight();
        return Theme::Spacing::M * 2.0f + inputRowHeight + style.ItemSpacing.y + controlsRowHeight;
    }
} // namespace

AIChatPanel::AIChatPanel(AIChatState* chatState)
    : chatState_(chatState), client_(std::make_unique<AIClient>()) {}

void AIChatPanel::setInsertCallback(InsertSQLCallback cb) {
    insertCallback_ = std::move(cb);
}

void AIChatPanel::render() {
    pollStreaming();

    const auto& colors = Application::getInstance().getCurrentColors();

    // Header buttons
    float btnWidth = ImGui::CalcTextSize(LABEL_SETTINGS).x +
                     ImGui::CalcTextSize(LABEL_CLEAR_CHAT).x +
                     ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - btnWidth);
    if (ImGui::SmallButton(LABEL_SETTINGS)) {
        AISettingsDialog::instance().show();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(LABEL_CLEAR_CHAT)) {
        chatState_->clear();
        if (client_->isStreaming()) {
            client_->cancel();
        }
    }
    ImGui::Separator();

    // Messages area - reserve exact space for input section below
    float inputAreaHeight = getAIInputContainerHeight() + Theme::Spacing::S;
    float availHeight = ImGui::GetContentRegionAvail().y - inputAreaHeight;

    if (ImGui::BeginChild("##ai_messages", ImVec2(-1, availHeight), false)) {
        if (chatState_->getMessages().empty()) {
            // Center the placeholder text
            ImVec2 textSize = ImGui::CalcTextSize(LABEL_EMPTY_CHAT, nullptr, false,
                                                  ImGui::GetContentRegionAvail().x * 0.8f);
            float availW = ImGui::GetContentRegionAvail().x;
            float availH = ImGui::GetContentRegionAvail().y;
            ImGui::SetCursorPos(ImVec2((availW - textSize.x) * 0.5f, (availH - textSize.y) * 0.5f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availW * 0.8f);
            ImGui::TextColored(colors.subtext0, "%s", LABEL_EMPTY_CHAT);
            ImGui::PopTextWrapPos();
        } else {
            renderMessages();
        }

        if (scrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom_ = false;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));

    // Input area
    renderInputArea();
}

void AIChatPanel::renderMessages() {
    const auto& messages = chatState_->getMessages();
    for (size_t i = 0; i < messages.size(); ++i) {
        renderMessage(messages[i], i);
    }

    // Show spinner during streaming
    if (client_->isStreaming()) {
        ImGui::Spacing();
        UIUtils::Spinner("##ai_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }
}

void AIChatPanel::renderMessage(const AIChatMessage& msg, size_t index) {
    const auto& colors = Application::getInstance().getCurrentColors();
    bool isUser = (msg.role == "user");

    ImGui::PushID(static_cast<int>(index));

    // Bottom padding between messages
    if (index > 0) {
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    }

    // Horizontal padding for the whole message block
    ImGui::Indent(Theme::Spacing::M);

    // Role label
    ImGui::TextColored(colors.subtext0, "%s", isUser ? "You" : "Assistant");

    float wrapWidth = ImGui::GetContentRegionAvail().x - Theme::Spacing::M;

    // Parse and render content with code blocks
    auto blocks = parseCodeBlocks(msg.content);

    if (blocks.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextWrapped("%s", msg.content.c_str());
        ImGui::PopTextWrapPos();
    } else {
        size_t lastEnd = 0;
        size_t blockIdx = 0;
        for (const auto& block : blocks) {
            if (block.start > lastEnd) {
                std::string textBefore = msg.content.substr(lastEnd, block.start - lastEnd);
                while (!textBefore.empty() && textBefore.front() == '\n') {
                    textBefore.erase(textBefore.begin());
                }
                while (!textBefore.empty() && textBefore.back() == '\n') {
                    textBefore.pop_back();
                }
                if (!textBefore.empty()) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
                    ImGui::TextWrapped("%s", textBefore.c_str());
                    ImGui::PopTextWrapPos();
                }
            }

            renderCodeBlock(block.code, block.lang, index, blockIdx);
            lastEnd = block.end;
            ++blockIdx;
        }

        if (lastEnd < msg.content.size()) {
            std::string textAfter = msg.content.substr(lastEnd);
            while (!textAfter.empty() && textAfter.front() == '\n') {
                textAfter.erase(textAfter.begin());
            }
            if (!textAfter.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
                ImGui::TextWrapped("%s", textAfter.c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }

    ImGui::Unindent(Theme::Spacing::M);
    ImGui::PopID();
}

void AIChatPanel::renderCodeBlock(const std::string& code, const std::string& lang, size_t msgIdx,
                                  size_t blockIdx) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);

    std::string childId = std::format("##code_{}_{}", msgIdx, blockIdx);

    // Calculate needed height
    float lineCount = 1.0f;
    for (char c : code) {
        if (c == '\n') {
            lineCount += 1.0f;
        }
    }
    float textHeight = lineCount * ImGui::GetTextLineHeightWithSpacing();
    float totalHeight = textHeight + Theme::Spacing::M * 2 + ImGui::GetFrameHeightWithSpacing();

    if (ImGui::BeginChild(childId.c_str(), ImVec2(-1, totalHeight), true)) {
        // Action buttons
        std::string copyId = std::format("{}##{}_{}_{}", LABEL_COPY, "cp", msgIdx, blockIdx);
        if (ImGui::SmallButton(copyId.c_str())) {
            ImGui::SetClipboardText(code.c_str());
        }

        bool isSql = (lang == "sql" || lang == "SQL" || lang == "sqlite" || lang == "postgresql" ||
                      lang == "mysql" || lang.empty());

        if (isSql && insertCallback_) {
            ImGui::SameLine();
            std::string insertId =
                std::format("{}##{}_{}_{}", LABEL_INSERT, "ins", msgIdx, blockIdx);
            if (ImGui::SmallButton(insertId.c_str())) {
                insertCallback_(code);
            }
        }

        // Code text
        ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
        ImGui::TextWrapped("%s", code.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void AIChatPanel::renderInputArea() {
    if (!modelSettingsLoaded_) {
        loadModelSettings();
    }

    const auto& colors = Application::getInstance().getCurrentColors();

    // Bordered container for input area (sharp corners)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::M));

    float containerHeight = getAIInputContainerHeight();
    if (ImGui::BeginChild("##ai_input_container", ImVec2(-1, containerHeight),
                          ImGuiChildFlags_Borders)) {
        // Row 1: Full-width text input (no border)
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushItemWidth(-1);

        if (focusInput_) {
            ImGui::SetKeyboardFocusHere();
            focusInput_ = false;
        }

        bool submitted = ImGui::InputTextWithHint(
            "##ai_input", "Ask me anything about your database...", inputBuf_, sizeof(inputBuf_),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        // Row 2: AI icon + model dropdown (+ chevron) + send button
        float rowStartX = ImGui::GetCursorPosX();
        float rowLeftPadding = Theme::Spacing::S;
        float rowRightPadding = Theme::Spacing::S;

        // AI icon
        ImGui::SetCursorPosX(rowStartX + rowLeftPadding);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, ICON_FA_WAND_MAGIC_SPARKLES);
        ImGui::SameLine(0.0f, Theme::Spacing::S);

        // Model dropdown
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

        std::string modelPreview =
            std::format("{} {}", MODEL_OPTIONS[modelIndex_].label, ICON_FA_ANGLE_DOWN);
        ImGui::PushItemWidth(ImGui::CalcTextSize(modelPreview.c_str()).x +
                             ImGui::GetStyle().FramePadding.x * 2 + Theme::Spacing::S);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::S, Theme::Spacing::S));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::S));
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
        if (ImGui::BeginCombo("##ai_model", modelPreview.c_str(), ImGuiComboFlags_NoArrowButton)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, Theme::Spacing::S));
            for (int i = 0; i < MODEL_COUNT; ++i) {
                bool selected = (modelIndex_ == i);
                // Show provider prefix
                std::string label = MODEL_OPTIONS[i].label;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    modelIndex_ = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::PopStyleVar();
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        ImGui::PopItemWidth();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        // Send button (right-aligned)
        float sendBtnWidth = ImGui::GetFrameHeight(); // square button
        ImGui::SameLine(rowStartX + ImGui::GetContentRegionAvail().x - sendBtnWidth -
                        rowRightPadding);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);

        if (client_->isStreaming()) {
            if (ImGui::Button(ICON_FA_STOP, ImVec2(sendBtnWidth, 0))) {
                client_->cancel();
            }
        } else {
            if (ImGui::Button(ICON_FA_ARROW_UP, ImVec2(sendBtnWidth, 0)) || submitted) {
                sendMessage();
            }
        }

        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void AIChatPanel::sendMessage() {
    std::string text(inputBuf_);

    // Trim whitespace
    auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return;
    }
    text = text.substr(start);
    auto end = text.find_last_not_of(" \t\n\r");
    text = text.substr(0, end + 1);

    if (text.empty()) {
        return;
    }

    // Add user message
    chatState_->addUserMessage(text);
    std::vector<AIChatMessage> requestMessages = chatState_->getMessages();
    chatState_->startAssistantMessage();
    inputBuf_[0] = '\0';
    scrollToBottom_ = true;
    focusInput_ = true;

    auto* appState = Application::getInstance().getAppState();
    AIProvider provider = getSelectedProvider();
    std::string apiKey = appState->getSetting(getProviderApiKeySetting(provider), "");
    if (apiKey.empty()) {
        // Backward compatibility with older single-key setting.
        apiKey = appState->getSetting("ai_api_key", "");
    }

    if (apiKey.empty()) {
        chatState_->appendToAssistant("Please configure your API key in AI Settings first.");
        chatState_->finalizeAssistant();
        return;
    }

    std::string model = getSelectedModel();
    std::string systemPrompt = chatState_->buildSystemPrompt();
    client_->sendStreaming(provider, apiKey, model, systemPrompt, requestMessages);
}

void AIChatPanel::loadModelSettings() {
    auto* appState = Application::getInstance().getAppState();
    std::string provider = appState->getSetting("ai_provider", "anthropic");

    // Find matching model option based on saved provider
    if (provider == "gemini") {
        for (int i = 0; i < MODEL_COUNT; ++i) {
            if (MODEL_OPTIONS[i].provider == AIProvider::GEMINI) {
                modelIndex_ = i;
                break;
            }
        }
    }
    modelSettingsLoaded_ = true;
}

std::string AIChatPanel::getSelectedModel() const {
    return MODEL_OPTIONS[modelIndex_].model;
}

AIProvider AIChatPanel::getSelectedProvider() const {
    return MODEL_OPTIONS[modelIndex_].provider;
}

void AIChatPanel::pollStreaming() {
    if (!client_->isStreaming() && !client_->isDone()) {
        return;
    }

    std::string deltas = client_->drainDeltas();
    if (!deltas.empty()) {
        chatState_->appendToAssistant(deltas);
        scrollToBottom_ = true;
    }

    if (client_->consumeDone() && !client_->isStreaming()) {
        std::string error = client_->getError();
        if (!error.empty()) {
            if (!deltas.empty()) {
                chatState_->appendToAssistant("\n\n");
            }
            chatState_->appendToAssistant(std::format("Error: {}", error));
        }
        chatState_->finalizeAssistant();
        scrollToBottom_ = true;
    }
}

std::vector<AIChatPanel::CodeBlock> AIChatPanel::parseCodeBlocks(const std::string& content) {
    std::vector<CodeBlock> blocks;
    size_t pos = 0;

    while (pos < content.size()) {
        auto start = content.find("```", pos);
        if (start == std::string::npos) {
            break;
        }

        // Find language identifier (rest of line after ```)
        size_t langStart = start + 3;
        size_t langEnd = content.find('\n', langStart);
        if (langEnd == std::string::npos) {
            break;
        }

        std::string lang = content.substr(langStart, langEnd - langStart);
        // Trim whitespace from lang
        while (!lang.empty() && (lang.back() == ' ' || lang.back() == '\r')) {
            lang.pop_back();
        }

        // Find closing ```
        size_t codeStart = langEnd + 1;
        auto codeEnd = content.find("```", codeStart);
        if (codeEnd == std::string::npos) {
            // Unclosed code block - treat rest as code (streaming in progress)
            std::string code = content.substr(codeStart);
            while (!code.empty() && code.back() == '\n') {
                code.pop_back();
            }
            blocks.push_back({lang, code, start, content.size()});
            break;
        }

        std::string code = content.substr(codeStart, codeEnd - codeStart);
        // Trim trailing newline
        while (!code.empty() && code.back() == '\n') {
            code.pop_back();
        }

        // end position includes closing ```
        size_t blockEnd = codeEnd + 3;
        // Skip newline after closing ```
        if (blockEnd < content.size() && content[blockEnd] == '\n') {
            ++blockEnd;
        }

        blocks.push_back({lang, code, start, blockEnd});
        pos = blockEnd;
    }

    return blocks;
}
