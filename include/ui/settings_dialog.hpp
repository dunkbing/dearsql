#pragma once

#include <functional>
#include <string>

// shared ImGui settings dialog — opened by the "more" (hamburger) button on
// every platform, replacing the old native popovers. holds appearance settings.
class SettingsDialog {
public:
    static SettingsDialog& instance();

    void open();
    void render();

    // platform-native actions wired up during titlebar setup
    std::function<void()> onManageLicense;
    std::function<void()> onReportBug;

private:
    SettingsDialog();
    ~SettingsDialog();

    bool open_ = false;
    bool pendingOpen_ = false;

    // the `dearsql` shell command, checked when the dialog opens
    bool cliInstalled_ = false;
    std::string cliStatus_;
};
