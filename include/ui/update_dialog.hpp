#pragma once

#include <string>

class UpdateDialog {
public:
    static UpdateDialog& instance();

    UpdateDialog(const UpdateDialog&) = delete;
    UpdateDialog& operator=(const UpdateDialog&) = delete;

    void render();

    void showChecking();
    void showUpdateAvailable(const std::string& currentVersion, const std::string& newVersion,
                             const std::string& releaseNotes);
    void showUpToDate();
    void showDownloading();
    void showDownloadComplete();
    void showError(const std::string& message);

    [[nodiscard]] bool isOpen() const {
        return isOpen_;
    }
    [[nodiscard]] bool wantsDownload() const {
        return wantsDownload_;
    }
    [[nodiscard]] bool wantsRestart() const {
        return wantsRestart_;
    }
    void clearWantsDownload() {
        wantsDownload_ = false;
    }
    void clearWantsRestart() {
        wantsRestart_ = false;
    }

private:
    UpdateDialog() = default;
    ~UpdateDialog() = default;

    enum class State {
        Hidden,
        Checking,
        UpdateAvailable,
        UpToDate,
        Downloading,
        DownloadComplete,
        Error
    };

    State state_ = State::Hidden;
    bool isOpen_ = false;
    bool wantsDownload_ = false;
    bool wantsRestart_ = false;

    std::string currentVersion_;
    std::string newVersion_;
    std::string releaseNotes_;
    std::string errorMessage_;
};
