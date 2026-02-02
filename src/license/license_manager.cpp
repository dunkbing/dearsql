#include "license/license_manager.hpp"
#include "app_state.hpp"
#include "application.hpp"
#include "utils/logger.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <fstream>

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

    std::string storedKey = appState->getSetting("license_key", "");
    std::string storedInstanceId = appState->getSetting("license_instance_id", "");
    std::string storedStatus = appState->getSetting("license_status", "");
    std::string storedEmail = appState->getSetting("license_email", "");
    std::string storedActivatedAt = appState->getSetting("license_activated_at", "");

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

    appState->setSetting("license_key", license.licenseKey);
    appState->setSetting("license_instance_id", license.instanceId);
    appState->setSetting("license_status", license.status);
    appState->setSetting("license_email", license.customerEmail);
    appState->setSetting("license_activated_at", license.activatedAt);

    Logger::info("Stored license");
}

void LicenseManager::clearStoredLicense() {
    auto* appState = Application::getInstance().getAppState();

    appState->setSetting("license_key", "");
    appState->setSetting("license_instance_id", "");
    appState->setSetting("license_status", "");
    appState->setSetting("license_email", "");
    appState->setSetting("license_activated_at", "");

    currentLicense = LicenseInfo{};
    Logger::info("Cleared stored license");
}

LicenseInfo LicenseManager::doActivation(const std::string& licenseKey,
                                         const std::string& instanceId) {
    LicenseInfo result;

    httplib::Client cli("https://api.lemonsqueezy.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["license_key"] = licenseKey;
    body["instance_name"] = instanceId;

    auto res = cli.Post("/v1/licenses/activate", body.dump(), "application/json");

    if (!res) {
        result.error = "Network error: could not connect to license server";
        return result;
    }

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
        } else {
            result.error = respJson.value("error", "Activation failed");
        }
    } catch (const std::exception& e) {
        result.error = std::string("Failed to parse response: ") + e.what();
    }

    return result;
}

LicenseInfo LicenseManager::doDeactivation(const std::string& licenseKey,
                                           const std::string& instanceId) {
    LicenseInfo result;

    httplib::Client cli("https://api.lemonsqueezy.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);

    json body;
    body["license_key"] = licenseKey;
    body["instance_id"] = instanceId;

    auto res = cli.Post("/v1/licenses/deactivate", body.dump(), "application/json");

    if (!res) {
        result.error = "Network error: could not connect to license server";
        return result;
    }

    if (res->status == 200) {
        result.valid = false;
        result.status = "deactivated";
    } else {
        result.error = "Deactivation failed (HTTP " + std::to_string(res->status) + ")";
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
