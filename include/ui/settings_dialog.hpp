#pragma once

#include <memory>
#include <mutex>
#include <string>

// shared ImGui settings dialog — opened by the "more" (hamburger) button on
// every platform, replacing the old native popovers. holds appearance settings.
class SettingsDialog {
public:
    static SettingsDialog& instance();

    void open();
    void render();

private:
    SettingsDialog();
    ~SettingsDialog();

    // manage license, a modal over the settings
    void renderLicense();

    bool open_ = false;
    bool pendingOpen_ = false;

    // the `dearsql` shell command, checked when the dialog opens
    bool cliInstalled_ = false;
    std::string cliStatus_;
    std::string keyStoreStatus_;

    // the license dialog; activation answers on a worker thread
    struct LicenseResult {
        std::mutex mutex;
        bool done = false;
        std::string error;
    };
    bool licenseOpen_ = false;
    char licenseKey_[128] = {};
    std::string licenseStatus_;
    bool licenseError_ = false;
    std::shared_ptr<LicenseResult> licenseResult_;
};
