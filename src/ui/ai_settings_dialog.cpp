#include "ui/ai_settings_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <cfloat>
#include <cstring>

namespace {
    constexpr const char* POPUP_ID = "AI Settings";
    constexpr const char* PROVIDER_LABELS[] = {"Anthropic", "Gemini"};
    constexpr const char* PROVIDER_VALUES[] = {"anthropic", "gemini"};
    constexpr int PROVIDER_COUNT = sizeof(PROVIDER_VALUES) / sizeof(PROVIDER_VALUES[0]);
} // namespace

AISettingsDialog& AISettingsDialog::instance() {
    static AISettingsDialog dialog;
    return dialog;
}

void AISettingsDialog::show() {
    needsLoad_ = true;
    isDialogOpen_ = true;
    pendingOpen_ = true;
}

void AISettingsDialog::loadSettings() {
    auto* appState = Application::getInstance().getAppState();

    std::string provider = appState->getSetting("ai_provider", "anthropic");
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

    needsLoad_ = false;
}

void AISettingsDialog::saveSettings() {
    auto* appState = Application::getInstance().getAppState();
    appState->setSetting("ai_provider", PROVIDER_VALUES[providerIndex_]);
    appState->setSetting(getSelectedProviderSettingKey(), apiKeyBuf_);
}

const char* AISettingsDialog::getSelectedProviderSettingKey() const {
    return providerIndex_ == 1 ? "ai_api_key_gemini" : "ai_api_key_anthropic";
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

    if (ImGui::BeginPopupModal(POPUP_ID, &isDialogOpen_)) {
        if (needsLoad_) {
            loadSettings();
        }

        const auto& colors = Application::getInstance().getCurrentColors();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::L, Theme::Spacing::L));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
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
        if (ImGui::Combo("##ai_provider", &providerIndex_, PROVIDER_LABELS, PROVIDER_COUNT) &&
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

        // Save / Cancel
        float buttonWidth = (ImGui::GetContentRegionAvail().x - Theme::Spacing::M) * 0.5f;
        if (ImGui::Button("Save", ImVec2(buttonWidth, 0))) {
            saveSettings();
            isDialogOpen_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
            isDialogOpen_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(4);

        ImGui::EndPopup();
    }
}
