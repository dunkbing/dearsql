#include "ui/ai_settings_dialog.hpp"
#include "ai/ai_client.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"
#include "utils/select.hpp"
#include <cfloat>
#include <cstring>

namespace {
    constexpr const char* POPUP_ID = "AI Settings";
    constexpr const char* PROVIDER_LABELS[] = {"Anthropic", "OpenAI", "Gemini"};
    constexpr const char* PROVIDER_VALUES[] = {"anthropic", "openai", "gemini"};
    constexpr int PROVIDER_COUNT = sizeof(PROVIDER_VALUES) / sizeof(PROVIDER_VALUES[0]);
} // namespace

AISettingsDialog& AISettingsDialog::instance() {
    static AISettingsDialog dialog;
    return dialog;
}

void AISettingsDialog::show(const std::string& provider) {
    preselectProvider_ = provider;
    needsLoad_ = true;
    isDialogOpen_ = true;
    pendingOpen_ = true;
}

void AISettingsDialog::loadSettings() {
    auto* appState = Application::getInstance().getAppState();

    const std::string provider = preselectProvider_.empty()
                                     ? appState->getSetting("ai_provider", "anthropic")
                                     : preselectProvider_;
    providerIndex_ = 0;
    for (int i = 0; i < PROVIDER_COUNT; ++i) {
        if (provider == PROVIDER_VALUES[i]) {
            providerIndex_ = i;
            break;
        }
    }

    std::string apiKey = appState->getSetting(getSelectedProviderSettingKey(), "");
    if (apiKey.empty()) {
        // Backward compatibility with older single-key setting.
        apiKey = appState->getSetting("ai_api_key", "");
    }
    std::strncpy(apiKeyBuf_, apiKey.c_str(), sizeof(apiKeyBuf_) - 1);
    apiKeyBuf_[sizeof(apiKeyBuf_) - 1] = '\0';

    mcpEnabled_ = appState->getSetting("ai_mcp_enabled", "1") == "1";

    needsLoad_ = false;
}

void AISettingsDialog::saveSettings() {
    auto* appState = Application::getInstance().getAppState();
    appState->setSetting("ai_provider", PROVIDER_VALUES[providerIndex_]);
    appState->setSetting(getSelectedProviderSettingKey(), apiKeyBuf_);
    appState->setSetting("ai_mcp_enabled", mcpEnabled_ ? "1" : "0");
}

const char* AISettingsDialog::getSelectedProviderSettingKey() const {
    return apiKeySettingFor(static_cast<AIProvider>(providerIndex_)); // same order as the enum
}

void AISettingsDialog::render() {
    if (!isDialogOpen_) {
        return;
    }

    if (pendingOpen_) {
        ImGui::OpenPopup(POPUP_ID);
        pendingOpen_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));

    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::CornerRadius::MEDIUM);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface2);
    if (ImGui::BeginPopupModal(POPUP_ID, &isDialogOpen_, ImGuiWindowFlags_NoResize)) {
        if (needsLoad_) {
            loadSettings();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::L, Theme::Spacing::L));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface1);

        // API Key
        ImGui::Text("API Key");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##ai_api_key", apiKeyBuf_, sizeof(apiKeyBuf_),
                         ImGuiInputTextFlags_Password);

        ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
        ImGui::Text("Provider");
        ImGui::SetNextItemWidth(-1);
        int previousProviderIndex = providerIndex_;
        if (UIUtils::Select("##ai_provider", &providerIndex_, PROVIDER_LABELS, PROVIDER_COUNT) &&
            providerIndex_ != previousProviderIndex) {
            auto* appState = Application::getInstance().getAppState();
            std::string apiKey = appState->getSetting(getSelectedProviderSettingKey(), "");
            if (apiKey.empty()) {
                apiKey = appState->getSetting("ai_api_key", "");
            }
            std::strncpy(apiKeyBuf_, apiKey.c_str(), sizeof(apiKeyBuf_) - 1);
            apiKeyBuf_[sizeof(apiKeyBuf_) - 1] = '\0';
        }

        ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));

        // Agent access to the database (MCP tools)
        ImGui::Text("Agent access");
        ImGui::Checkbox("Let the agent query my database", &mcpEnabled_);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped(
            "Gives coding agents a read-only tool to list tables and run SELECT queries "
            "against the connected database. Results are sent to the agent's model "
            "provider. Applies to the next session.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));

        // Save / Cancel
        float buttonWidth = (ImGui::GetContentRegionAvail().x - Theme::Spacing::M) * 0.5f;
        if (UIUtils::Button("Save", UIUtils::ButtonVariant::Primary, ImVec2(buttonWidth, 0))) {
            saveSettings();
            isDialogOpen_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (UIUtils::Button("Cancel", UIUtils::ButtonVariant::Secondary, ImVec2(buttonWidth, 0))) {
            isDialogOpen_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}
