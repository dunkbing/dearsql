#include "license/license_manager.hpp"
#include "app_state.hpp"
#include "application.hpp"
#include "utils/logger.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <fstream>

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
    return currentLicense.valid && currentLicense.status == "active";
}

const LicenseInfo& LicenseManager::getLicenseInfo() const {
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

    currentLicense = LicenseInfo{};
    Logger::info("Cleared stored license");
}

LicenseInfo LicenseManager::doActivation(const std::string& licenseKey,
                                         const std::string& instanceId) {
    LicenseInfo result;

    Logger::info("License activation: starting for instance: " + instanceId);

    httplib::Client cli("https://api.lemonsqueezy.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["license_key"] = licenseKey;
    body["instance_name"] = instanceId;

    Logger::info("License activation: sending request to /v1/licenses/activate");
    Logger::info("License activation: request body: " + body.dump());

    auto res = cli.Post("/v1/licenses/activate", body.dump(), "application/json");

    if (!res) {
        Logger::error("License activation: network error - could not connect");
        result.error = "Network error: could not connect to license server";
        return result;
    }

    Logger::info("License activation: HTTP status: " + std::to_string(res->status));
    Logger::info("License activation: response body: " + res->body);

    if (res->status != 200) {
        try {
            auto respJson = json::parse(res->body);
            if (respJson.contains("error")) {
                result.error = respJson["error"].get<std::string>();
            } else {
                result.error = "Activation failed (HTTP " + std::to_string(res->status) + ")";
            }
        } catch (...) {
            result.error = "Activation failed (HTTP " + std::to_string(res->status) + ")";
        }
        Logger::error("License activation: failed - " + result.error);
        return result;
    }

    try {
        auto respJson = json::parse(res->body);

        if (respJson.value("activated", false) || respJson.value("valid", false)) {
            result.valid = true;
            result.licenseKey = licenseKey;
            result.instanceId = respJson.value("instance", json::object()).value("id", instanceId);
            result.status = "active";

            if (respJson.contains("meta")) {
                auto& meta = respJson["meta"];
                result.customerEmail = meta.value("customer_email", "");
                result.productName = meta.value("product_name", "");
            }

            if (respJson.contains("license_key")) {
                auto& lk = respJson["license_key"];
                result.activationLimit = lk.value("activation_limit", 0);
                result.activationsCount = lk.value("activations_count", 0);
                result.status = lk.value("status", "active");
                result.expiresAt = lk.value("expires_at", "");
            }

            result.activatedAt = respJson.value("instance", json::object()).value("created_at", "");
            Logger::info("License activation: success - status: " + result.status);
        } else {
            result.error = respJson.value("error", "Activation failed");
            Logger::error("License activation: API returned failure - " + result.error);
        }
    } catch (const std::exception& e) {
        result.error = std::string("Failed to parse response: ") + e.what();
        Logger::error("License activation: parse error - " + result.error);
    }

    return result;
}

LicenseInfo LicenseManager::doDeactivation(const std::string& licenseKey,
                                           const std::string& instanceId) {
    LicenseInfo result;

    Logger::info("License deactivation: starting for instance: " + instanceId);

    httplib::Client cli("https://api.lemonsqueezy.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["license_key"] = licenseKey;
    body["instance_id"] = instanceId;

    Logger::info("License deactivation: sending request to /v1/licenses/deactivate");
    Logger::info("License deactivation: request body: " + body.dump());

    auto res = cli.Post("/v1/licenses/deactivate", body.dump(), "application/json");

    if (!res) {
        Logger::error("License deactivation: network error - could not connect");
        result.error = "Network error: could not connect to license server";
        return result;
    }

    Logger::info("License deactivation: HTTP status: " + std::to_string(res->status));
    Logger::info("License deactivation: response body: " + res->body);

    if (res->status == 200) {
        result.valid = false;
        result.status = "deactivated";
        Logger::info("License deactivation: success");
    } else {
        try {
            auto respJson = json::parse(res->body);
            if (respJson.contains("error")) {
                result.error = respJson["error"].get<std::string>();
            } else {
                result.error = "Deactivation failed (HTTP " + std::to_string(res->status) + ")";
            }
        } catch (...) {
            result.error = "Deactivation failed (HTTP " + std::to_string(res->status) + ")";
        }
        Logger::error("License deactivation: failed - " + result.error);
    }

    return result;
}

LicenseInfo LicenseManager::doValidation(const std::string& licenseKey,
                                         const std::string& instanceId) {
    LicenseInfo result;

    httplib::Client cli("https://api.lemonsqueezy.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["license_key"] = licenseKey;
    body["instance_id"] = instanceId;

    auto res = cli.Post("/v1/licenses/validate", body.dump(), "application/json");

    if (!res) {
        result.error = "Network error";
        return result;
    }

    if (res->status == 200) {
        try {
            auto respJson = json::parse(res->body);
            result.valid = respJson.value("valid", false);
            result.licenseKey = licenseKey;
            result.instanceId = instanceId;

            if (respJson.contains("license_key")) {
                result.status = respJson["license_key"].value("status", "");
            }

            if (!result.valid || result.status != "active") {
                result.error = "License is no longer valid";
            }
        } catch (...) {
            result.error = "Failed to parse validation response";
        }
    } else {
        result.error = "Validation failed";
    }

    return result;
}

void LicenseManager::activateLicense(const std::string& licenseKey, ActivationCallback callback) {
    if (activating) {
        LicenseInfo err;
        err.error = "Activation already in progress";
        callback(err);
        return;
    }

    activating = true;
    std::string instanceId = getInstanceId();

    std::thread([this, licenseKey, instanceId, callback]() {
        auto result = doActivation(licenseKey, instanceId);

        if (result.valid) {
            currentLicense = result;
            storeLicense(result);
        }

        activating = false;
        callback(result);
    }).detach();
}

void LicenseManager::deactivateLicense(ActivationCallback callback) {
    if (!currentLicense.valid || currentLicense.licenseKey.empty()) {
        LicenseInfo err;
        err.error = "No active license to deactivate";
        callback(err);
        return;
    }

    activating = true;
    std::string key = currentLicense.licenseKey;
    std::string instanceId = currentLicense.instanceId;

    std::thread([this, key, instanceId, callback]() {
        auto result = doDeactivation(key, instanceId);

        if (result.error.empty()) {
            clearStoredLicense();
        }

        activating = false;
        callback(result);
    }).detach();
}

void LicenseManager::validateLicense(ActivationCallback callback) {
    if (!currentLicense.valid || currentLicense.licenseKey.empty()) {
        LicenseInfo err;
        err.error = "No license to validate";
        callback(err);
        return;
    }

    std::string key = currentLicense.licenseKey;
    std::string instanceId = currentLicense.instanceId;

    std::thread([this, key, instanceId, callback]() {
        auto result = doValidation(key, instanceId);

        if (!result.valid) {
            clearStoredLicense();
        }

        callback(result);
    }).detach();
}
