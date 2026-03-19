#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

struct LicenseInfo {
    bool valid = false;
    bool networkError =
        false; // true when validation failed due to connectivity, not server rejection
    std::string licenseKey;
    std::string instanceId; // activation UUID returned by Polar on activation
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
    [[nodiscard]] LicenseInfo getLicenseInfo() const;
    void loadStoredLicense();
    // background-validates the stored license; only clears it if the server explicitly rejects it
    void validateStoredLicense();

    void activateLicense(const std::string& licenseKey, ActivationCallback callback);
    void deactivateLicense(ActivationCallback callback);
    void validateLicense(ActivationCallback callback);

    [[nodiscard]] bool isActivating() const {
        return activating.load();
    }

    // generate a unique instance ID for this machine
    [[nodiscard]] std::string getInstanceId() const;

private:
    LicenseManager() = default;
    ~LicenseManager() = default;

    mutable std::mutex licenseMutex_;
    LicenseInfo currentLicense;
    std::atomic<bool> activating{false};

    void storeLicense(const LicenseInfo& license);
    void clearStoredLicense();

    LicenseInfo doActivation(const std::string& licenseKey, const std::string& instanceLabel);
    LicenseInfo doDeactivation(const std::string& licenseKey, const std::string& activationId);
    LicenseInfo doValidation(const std::string& licenseKey, const std::string& activationId);
};
