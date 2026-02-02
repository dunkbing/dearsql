#pragma once

#include <string>

class LicenseDialog {
public:
    static LicenseDialog& instance();

    LicenseDialog(const LicenseDialog&) = delete;
    LicenseDialog& operator=(const LicenseDialog&) = delete;

    void show();
    void render();

    [[nodiscard]] bool isOpen() const {
        return isDialogOpen;
    }

private:
    LicenseDialog() = default;
    ~LicenseDialog() = default;

    bool isDialogOpen = false;
    char licenseKeyBuffer[256] = {0};
    std::string errorMessage;
    std::string successMessage;
    bool isActivating = false;

    void reset();
    void renderLicensed();
    void renderUnlicensed();
};
