#pragma once

#include <functional>
#include <string>

struct LicenseInfo {
    bool valid = false;
    std::string licenseKey;
    std::string instanceId;
    std::string customerEmail;
    std::string productName;
    std::string status; // "active", "inactive", "expired", "disabled"
    int activationLimit = 0;
    int activationsCount = 0;
    std::string activatedAt;
    std::string expiresAt;
    std::string error;
};

class LicenseManager {
public:
    using ActivationCallback = std::function<void(const LicenseInfo&)>;

    static LicenseManager& instance();

    LicenseManager(const LicenseManager&) = delete;
    LicenseManager& operator=(const LicenseManager&) = delete;

    [[nodiscard]] bool hasValidLicense() const;
    [[nodiscard]] const LicenseInfo& getLicenseInfo() const;
    void loadStoredLicense();

    void activateLicense(const std::string& licenseKey, ActivationCallback callback);
    void deactivateLicense(ActivationCallback callback);
    void validateLicense(ActivationCallback callback);

    [[nodiscard]] bool isActivating() const {
        return activating;
    }

    // Generate a unique instance ID for this machine
    [[nodiscard]] std::string getInstanceId() const;

private:
    LicenseManager() = default;
    ~LicenseManager() = default;

    LicenseInfo currentLicense;
    bool activating = false;

    // Store license locally
    void storeLicense(const LicenseInfo& license);

    // Clear stored license
    void clearStoredLicense();

    // HTTP request to Lemon Squeezy API
    LicenseInfo doActivation(const std::string& licenseKey, const std::string& instanceId);
    LicenseInfo doDeactivation(const std::string& licenseKey, const std::string& instanceId);
    LicenseInfo doValidation(const std::string& licenseKey, const std::string& instanceId);
};
