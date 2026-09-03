#pragma once

#include <string>

class AISettingsDialog {
public:
    static AISettingsDialog& instance();

    AISettingsDialog(const AISettingsDialog&) = delete;
    AISettingsDialog& operator=(const AISettingsDialog&) = delete;

    // provider: "anthropic" / "openai" / "gemini" to preselect; empty keeps the saved one
    void show(const std::string& provider = "");
    void render();
    [[nodiscard]] bool isOpen() const {
        return isDialogOpen_;
    }

private:
    AISettingsDialog() = default;
    ~AISettingsDialog() = default;

    bool isDialogOpen_ = false;
    bool pendingOpen_ = false;
    bool needsLoad_ = true;
    std::string preselectProvider_;

    char apiKeyBuf_[256] = {};
    int providerIndex_ = 0;
    bool mcpEnabled_ = true;

    void loadSettings();
    void saveSettings();
    [[nodiscard]] const char* getSelectedProviderSettingKey() const;
};
