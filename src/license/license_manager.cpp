#include "license/license_manager.hpp"
#include "app_state.hpp"
#include "application.hpp"
#include "config.hpp"
#include "utils/logger.hpp"

#include <fstream>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {
    constexpr const char* kSettingLicenseKey = "license_key";
    constexpr const char* kSettingInstanceId = "license_instance_id";
    constexpr const char* kSettingStatus = "license_status";
    constexpr const char* kSettingEmail = "license_email";
    constexpr const char* kSettingActivatedAt = "license_activated_at";

} // namespace

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

LicenseManager& LicenseManager::instance() {
    static LicenseManager inst;
    return inst;
}

bool LicenseManager::hasValidLicense() const {
    std::lock_guard lock(licenseMutex_);
    return currentLicense.valid && currentLicense.status == "active";
}

LicenseInfo LicenseManager::getLicenseInfo() const {
    std::lock_guard lock(licenseMutex_);
    return currentLicense;
}

std::string LicenseManager::getInstanceId() const {
    // Generate a stable machine identifier
    std::string machineId;

#ifdef __linux__
    // Try to read machine-id on Linux
    std::ifstream f("/etc/machine-id");
    if (f.is_open()) {
        std::getline(f, machineId);
        f.close();
    }
#elif __APPLE__
    // Use system_profiler on macOS
    FILE* pipe = popen(
        "ioreg -rd1 -c IOPlatformExpertDevice | awk '/IOPlatformUUID/ { print $3 }' | tr -d '\"'",
        "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe)) {
            machineId = buffer;
            // Remove trailing newline
            if (!machineId.empty() && machineId.back() == '\n') {
                machineId.pop_back();
            }
        }
        pclose(pipe);
    }
#elif _WIN32
    // Use Windows machine GUID
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ,
                      &hKey) == ERROR_SUCCESS) {
        char value[256];
        DWORD size = sizeof(value);
        if (RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr, (LPBYTE)value, &size) ==
            ERROR_SUCCESS) {
            machineId = value;
        }
        RegCloseKey(hKey);
    }
#endif

    if (machineId.empty()) {
        // Fallback: use hostname
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            machineId = hostname;
        } else {
            machineId = "unknown-machine";
        }
    }

    return machineId;
}

void LicenseManager::loadStoredLicense() {
    auto* appState = Application::getInstance().getAppState();

    std::string storedKey = appState->getSetting(kSettingLicenseKey, "");
    std::string storedInstanceId = appState->getSetting(kSettingInstanceId, "");
    std::string storedStatus = appState->getSetting(kSettingStatus, "");
    std::string storedEmail = appState->getSetting(kSettingEmail, "");
    std::string storedActivatedAt = appState->getSetting(kSettingActivatedAt, "");

    if (!storedKey.empty() && storedStatus == "active") {
        std::lock_guard lock(licenseMutex_);
        currentLicense.valid = true;
        currentLicense.licenseKey = storedKey;
        currentLicense.instanceId = storedInstanceId;
        currentLicense.status = storedStatus;
        currentLicense.customerEmail = storedEmail;
        currentLicense.activatedAt = storedActivatedAt;

        Logger::info("Loaded stored license");
    }
}

void LicenseManager::storeLicense(const LicenseInfo& license) {
    auto* appState = Application::getInstance().getAppState();

    appState->setSetting(kSettingLicenseKey, license.licenseKey);
    appState->setSetting(kSettingInstanceId, license.instanceId);
    appState->setSetting(kSettingStatus, license.status);
    appState->setSetting(kSettingEmail, license.customerEmail);
    appState->setSetting(kSettingActivatedAt, license.activatedAt);

    Logger::info("Stored license");
}

void LicenseManager::clearStoredLicense() {
    auto* appState = Application::getInstance().getAppState();

    appState->setSetting(kSettingLicenseKey, "");
    appState->setSetting(kSettingInstanceId, "");
    appState->setSetting(kSettingStatus, "");
    appState->setSetting(kSettingEmail, "");
    appState->setSetting(kSettingActivatedAt, "");

    {
        std::lock_guard lock(licenseMutex_);
        currentLicense = LicenseInfo{};
    }
    Logger::info("Cleared stored license");
}

LicenseInfo LicenseManager::doActivation(const std::string& licenseKey,
                                         const std::string& instanceLabel) {
    LicenseInfo result;

    Logger::info("License activation: starting for label: " + instanceLabel);

    httplib::Client cli("https://api.polar.sh");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["key"] = licenseKey;
    body["organization_id"] = POLAR_ORGANIZATION_ID;
    body["label"] = instanceLabel;

    auto res =
        cli.Post("/v1/customer-portal/license-keys/activate", body.dump(), "application/json");

    if (!res) {
        Logger::error("License activation: network error");
        result.error = "Network error: could not connect to license server";
        return result;
    }

    Logger::info("License activation: HTTP status: " + std::to_string(res->status));

    if (res->status != 200) {
        try {
            auto respJson = json::parse(res->body);
            result.error = respJson.value("detail", "Activation failed (HTTP " +
                                                        std::to_string(res->status) + ")");
        } catch (...) {
            result.error = "Activation failed (HTTP " + std::to_string(res->status) + ")";
        }
        Logger::error("License activation: failed - " + result.error);
        return result;
    }

    try {
        auto respJson = json::parse(res->body);

        // activation UUID — required for deactivation and validation
        result.instanceId = respJson.value("id", "");

        auto& lk = respJson["license_key"];
        result.licenseKey = licenseKey;
        result.status = lk.value("status", "");
        result.expiresAt = lk.contains("expires_at") && !lk["expires_at"].is_null()
                               ? lk["expires_at"].get<std::string>()
                               : "";
        result.activationLimit = lk.value("limit_activations", 0);
        result.activationsCount = lk.value("activations", 0);
        result.activatedAt = respJson.value("created_at", "");

        if (respJson["license_key"].contains("customer")) {
            result.customerEmail = respJson["license_key"]["customer"].value("email", "");
        }

        if (result.status == "granted") {
            result.valid = true;
            result.status = "active";
            Logger::info("License activation: success, activation id: " + result.instanceId);
        } else {
            result.error = "License is not active (status: " + result.status + ")";
            Logger::error("License activation: " + result.error);
        }
    } catch (const std::exception& e) {
        result.error = std::string("Failed to parse response: ") + e.what();
        Logger::error("License activation: parse error - " + result.error);
    }

    return result;
}

LicenseInfo LicenseManager::doDeactivation(const std::string& licenseKey,
                                           const std::string& activationId) {
    LicenseInfo result;

    Logger::info("License deactivation: starting for activation: " + activationId);

    httplib::Client cli("https://api.polar.sh");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["key"] = licenseKey;
    body["organization_id"] = POLAR_ORGANIZATION_ID;
    body["activation_id"] = activationId;

    auto res =
        cli.Post("/v1/customer-portal/license-keys/deactivate", body.dump(), "application/json");

    if (!res) {
        Logger::error("License deactivation: network error");
        result.error = "Network error: could not connect to license server";
        return result;
    }

    Logger::info("License deactivation: HTTP status: " + std::to_string(res->status));

    if (res->status == 204) {
        result.valid = false;
        result.status = "deactivated";
        Logger::info("License deactivation: success");
    } else {
        try {
            auto respJson = json::parse(res->body);
            result.error = respJson.value("detail", "Deactivation failed (HTTP " +
                                                        std::to_string(res->status) + ")");
        } catch (...) {
            result.error = "Deactivation failed (HTTP " + std::to_string(res->status) + ")";
        }
        Logger::error("License deactivation: failed - " + result.error);
    }

    return result;
}

LicenseInfo LicenseManager::doValidation(const std::string& licenseKey,
                                         const std::string& activationId) {
    LicenseInfo result;

    httplib::Client cli("https://api.polar.sh");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["key"] = licenseKey;
    body["organization_id"] = POLAR_ORGANIZATION_ID;
    if (!activationId.empty()) {
        body["activation_id"] = activationId;
    }

    auto res =
        cli.Post("/v1/customer-portal/license-keys/validate", body.dump(), "application/json");

    if (!res) {
        result.error = "Network error";
        result.networkError = true;
        return result;
    }

    if (res->status == 200) {
        try {
            auto respJson = json::parse(res->body);

            std::string status = respJson.value("status", "");
            result.licenseKey = licenseKey;
            result.instanceId = activationId;
            result.status = (status == "granted") ? "active" : status;
            result.valid = (status == "granted");
            result.expiresAt = respJson.contains("expires_at") && !respJson["expires_at"].is_null()
                                   ? respJson["expires_at"].get<std::string>()
                                   : "";

            if (respJson.contains("customer")) {
                result.customerEmail = respJson["customer"].value("email", "");
            }

            if (!result.valid) {
                result.error = "License is no longer valid (status: " + status + ")";
            }
        } catch (...) {
            result.error = "Failed to parse validation response";
        }
    } else {
        result.error = "Validation failed (HTTP " + std::to_string(res->status) + ")";
    }

    return result;
}

void LicenseManager::activateLicense(const std::string& licenseKey, ActivationCallback callback) {
    if (activating.load()) {
        LicenseInfo err;
        err.error = "Activation already in progress";
        callback(err);
        return;
    }

    activating.store(true);
    std::string instanceId = getInstanceId();

    std::thread([this, licenseKey, instanceId, callback]() {
        auto result = doActivation(licenseKey, instanceId);

        if (result.valid) {
            {
                std::lock_guard lock(licenseMutex_);
                currentLicense = result;
            }
            storeLicense(result);
        }

        activating.store(false);
        callback(result);
    }).detach();
}

void LicenseManager::deactivateLicense(ActivationCallback callback) {
    std::string key, instanceId;
    {
        std::lock_guard lock(licenseMutex_);
        if (!currentLicense.valid || currentLicense.licenseKey.empty()) {
            LicenseInfo err;
            err.error = "No active license to deactivate";
            callback(err);
            return;
        }
        key = currentLicense.licenseKey;
        instanceId = currentLicense.instanceId;
    }

    activating.store(true);

    std::thread([this, key, instanceId, callback]() {
        auto result = doDeactivation(key, instanceId);

        if (result.error.empty()) {
            clearStoredLicense();
        }

        activating.store(false);
        callback(result);
    }).detach();
}

void LicenseManager::validateLicense(ActivationCallback callback) {
    std::string key, instanceId;
    {
        std::lock_guard lock(licenseMutex_);
        if (!currentLicense.valid || currentLicense.licenseKey.empty()) {
            LicenseInfo err;
            err.error = "No license to validate";
            callback(err);
            return;
        }
        key = currentLicense.licenseKey;
        instanceId = currentLicense.instanceId;
    }

    std::thread([this, key, instanceId, callback]() {
        auto result = doValidation(key, instanceId);

        if (!result.valid && !result.networkError) {
            clearStoredLicense();
        }

        callback(result);
    }).detach();
}

void LicenseManager::validateStoredLicense() {
    std::string key, instanceId;
    {
        std::lock_guard lock(licenseMutex_);
        if (!currentLicense.valid || currentLicense.licenseKey.empty()) {
            return;
        }
        key = currentLicense.licenseKey;
        instanceId = currentLicense.instanceId;
    }

    std::thread([this, key, instanceId]() {
        auto result = doValidation(key, instanceId);

        if (result.networkError) {
            Logger::info("Startup license validation: network unavailable, keeping cached license");
        } else if (!result.valid) {
            Logger::info("Startup license validation: server rejected license, clearing");
            clearStoredLicense();
        } else {
            Logger::info("Startup license validation: confirmed valid");
        }
    }).detach();
}
