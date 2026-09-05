#include "ui/ai_chat_panel.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/markdown_text.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <format>

namespace {
    constexpr const char* LABEL_CLEAR_CHAT = "Clear";
    constexpr const char* LABEL_SETTINGS = "Settings";
    constexpr const char* LABEL_EMPTY_CHAT = "Ask a question about your database or request SQL.";

    struct ModelOption {
        const char* label;
        const char* model;
        AIProvider provider;
    };

    constexpr ModelOption MODEL_OPTIONS[] = {
        {"claude-opus-5", "claude-opus-5", AIProvider::ANTHROPIC},
        {"claude-sonnet-5", "claude-sonnet-5", AIProvider::ANTHROPIC},
        {"claude-haiku-4-5", "claude-haiku-4-5", AIProvider::ANTHROPIC},
        {"gpt-5.6", "gpt-5.6", AIProvider::OPENAI},
        {"gpt-5.6-terra", "gpt-5.6-terra", AIProvider::OPENAI},
        {"gpt-5.6-luna", "gpt-5.6-luna", AIProvider::OPENAI},
        {"gemini-3.8-flash", "gemini-3.8-flash", AIProvider::GEMINI},
        {"gemini-3.1-pro", "gemini-3.1-pro-preview", AIProvider::GEMINI},
    };
    constexpr int MODEL_COUNT = sizeof(MODEL_OPTIONS) / sizeof(MODEL_OPTIONS[0]);

    const char* getProviderApiKeySetting(AIProvider provider) {
        return apiKeySettingFor(provider);
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
    chatState_->pollAsyncPrompt();
    pollStreaming();

    const auto& colors = Application::getInstance().getCurrentColors();

    float btnWidth = ImGui::CalcTextSize(LABEL_SETTINGS).x +
                     ImGui::CalcTextSize(LABEL_CLEAR_CHAT).x +
                     ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - btnWidth);
    if (UIUtils::SmallButton(LABEL_SETTINGS)) {
        AISettingsDialog::instance().show();
    }
    ImGui::SameLine();
    if (UIUtils::SmallButton(LABEL_CLEAR_CHAT)) {
        chatState_->cancelAsyncPrompt();
        if (client_->isStreaming()) {
            client_->cancel();
        }
        chatState_->clear();
    }
    ImGui::Separator();

    float inputAreaHeight = getAIInputContainerHeight() + Theme::Spacing::S;
    float availHeight = ImGui::GetContentRegionAvail().y - inputAreaHeight;

    if (ImGui::BeginChild("##ai_messages", ImVec2(-1, availHeight), false)) {
        if (chatState_->getMessages().empty()) {
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

    renderInputArea();
}

void AIChatPanel::renderMessages() {
    const auto& messages = chatState_->getMessages();
    for (size_t i = 0; i < messages.size(); ++i) {
        renderMessage(messages[i], i);
    }

    if (client_->isStreaming() || chatState_->isBuildingPrompt()) {
        ImGui::Spacing();
        UIUtils::Spinner("##ai_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        if (chatState_->isBuildingPrompt()) {
            ImGui::SameLine();
            ImGui::TextColored(Application::getInstance().getCurrentColors().subtext0,
                               "Analyzing database schema...");
        }
    }
}

void AIChatPanel::renderMessage(const AIChatMessage& msg, size_t index) {
    const auto& colors = Application::getInstance().getCurrentColors();
    bool isUser = (msg.role == "user");

    ImGui::PushID(static_cast<int>(index));

    if (index > 0) {
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    }

    ImGui::Indent(Theme::Spacing::M);
    ImGui::TextColored(colors.subtext0, "%s", isUser ? "You" : "Assistant");

    if (isUser) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                               Theme::Spacing::M);
        ImGui::TextWrapped("%s", msg.content.c_str());
        ImGui::PopTextWrapPos();
    } else {
        MarkdownText::render(msg.content, std::format("chat_{}", index), insertCallback_);
    }

    ImGui::Unindent(Theme::Spacing::M);
    ImGui::PopID();
}

void AIChatPanel::renderInputArea() {
    if (!modelSettingsLoaded_) {
        loadModelSettings();
    }

    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::M));

    float containerHeight = getAIInputContainerHeight();
    if (ImGui::BeginChild("##ai_input_container", ImVec2(-1, containerHeight),
                          ImGuiChildFlags_Borders)) {
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

        float rowStartX = ImGui::GetCursorPosX();
        float rowLeftPadding = Theme::Spacing::S;
        float rowRightPadding = Theme::Spacing::S;

        ImGui::SetCursorPosX(rowStartX + rowLeftPadding);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, ICON_FA_WAND_MAGIC_SPARKLES);
        ImGui::SameLine(0.0f, Theme::Spacing::S);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

        std::string modelPreview =
            std::format("{} {}", MODEL_OPTIONS[modelIndex_].label, ICON_FA_ANGLE_DOWN);
        ImGui::PushItemWidth(ImGui::CalcTextSize(modelPreview.c_str()).x +
                             ImGui::GetStyle().FramePadding.x * 2 + Theme::Spacing::S);
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
        ImGui::PopStyleVar(2);
        ImGui::PopItemWidth();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        float sendBtnWidth = ImGui::GetFrameHeight();
        ImGui::SameLine(rowStartX + ImGui::GetContentRegionAvail().x - sendBtnWidth -
                        rowRightPadding);

        if (client_->isStreaming() || chatState_->isBuildingPrompt()) {
            if (UIUtils::IconButton(ICON_FA_STOP, UIUtils::ButtonVariant::Danger,
                                    ImVec2(sendBtnWidth, 0))) {
                chatState_->cancelAsyncPrompt();
                client_->cancel();
            }
        } else {
            if (UIUtils::IconButton(ICON_FA_ARROW_UP, UIUtils::ButtonVariant::Primary,
                                    ImVec2(sendBtnWidth, 0)) ||
                submitted) {
                sendMessage();
            }
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void AIChatPanel::sendMessage() {
    std::string text(inputBuf_);

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

    if (chatState_->isBuildingPrompt()) {
        return; // Prevent multiple clicks
    }

    chatState_->buildSystemPromptAsync(
        [this, provider, apiKey, model, requestMessages](std::string systemPrompt) {
            client_->sendStreaming(provider, apiKey, model, systemPrompt, requestMessages);
        });
}

void AIChatPanel::loadModelSettings() {
    auto* appState = Application::getInstance().getAppState();
    std::string provider = appState->getSetting("ai_provider", "anthropic");

    const AIProvider wanted = provider == "gemini"   ? AIProvider::GEMINI
                              : provider == "openai" ? AIProvider::OPENAI
                                                     : AIProvider::ANTHROPIC;
    if (wanted != AIProvider::ANTHROPIC) {
        for (int i = 0; i < MODEL_COUNT; ++i) {
            if (MODEL_OPTIONS[i].provider == wanted) {
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

