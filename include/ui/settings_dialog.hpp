#pragma once

#include <functional>
#include <memory>
#include <string>

namespace dearsql {
    class TextEditor;
}

// shared ImGui settings dialog — opened by the "more" (hamburger) button on
// every platform, replacing the old native popovers. holds appearance settings
// and (macOS only) the custom-shader editor.
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

#if defined(__APPLE__)
    char shaderPath_[1024] = {};
    std::string shaderStatus_;
    std::unique_ptr<dearsql::TextEditor> shaderEditor_;
    void loadShaderState();
#endif
};
