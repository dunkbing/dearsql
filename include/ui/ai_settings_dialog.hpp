#pragma once

class AISettingsDialog {
public:
    static AISettingsDialog& instance();

    AISettingsDialog(const AISettingsDialog&) = delete;
    AISettingsDialog& operator=(const AISettingsDialog&) = delete;

    void show();
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

    char apiKeyBuf_[256] = {};
    int providerIndex_ = 0;

    void loadSettings();
    void saveSettings();
    [[nodiscard]] const char* getSelectedProviderSettingKey() const;
};
