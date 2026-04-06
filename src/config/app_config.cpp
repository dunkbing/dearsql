#include "config/app_config.hpp"
#include "utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <toml.hpp>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#else
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
    std::string getConfigFilePath() {
        std::filesystem::path configDir;

#ifdef _WIN32
        PWSTR appDataPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
            configDir = std::filesystem::path(appDataPath) / "DearSQL";
            CoTaskMemFree(appDataPath);
        } else {
            configDir = std::filesystem::current_path() / ".dearsql";
        }
#elif defined(__APPLE__)
        const char* homeDir = getenv("HOME");
        if (homeDir) {
            configDir = std::filesystem::path(homeDir) / "Library/Application Support/DearSQL";
        } else {
            configDir = std::filesystem::current_path() / ".dearsql";
        }
#else
        const char* xdgConfigHome = getenv("XDG_CONFIG_HOME");
        if (xdgConfigHome) {
            configDir = std::filesystem::path(xdgConfigHome) / "dearsql";
        } else {
            const char* homeDir = getenv("HOME");
            if (!homeDir) {
                struct passwd* pw = getpwuid(getuid());
                if (pw) {
                    homeDir = pw->pw_dir;
                }
            }
            if (homeDir) {
                configDir = std::filesystem::path(homeDir) / ".config" / "dearsql";
            } else {
                configDir = std::filesystem::current_path() / ".dearsql";
            }
        }
#endif

        // Ensure the directory exists
        std::error_code ec;
        if (!std::filesystem::exists(configDir, ec)) {
            std::filesystem::create_directories(configDir, ec);
        }

        return (configDir / "config.toml").string();
    }
} // namespace

namespace AppConfig {

    std::string getSetting(const std::string& key, const std::string& defaultValue) {
        std::string configPath = getConfigFilePath();
        if (!std::filesystem::exists(configPath)) {
            return defaultValue;
        }

        try {
            auto data = toml::parse(configPath);
            if (data.as_table().count(key)) {
                return toml::find<std::string>(data, key);
            }
        } catch (const std::exception& e) {
            Logger::error(std::string("Failed to read config.toml: ") + e.what());
        }

        return defaultValue;
    }

    bool setSetting(const std::string& key, const std::string& value) {
        std::string configPath = getConfigFilePath();
        toml::value data;

        if (std::filesystem::exists(configPath)) {
            try {
                data = toml::parse(configPath);
            } catch (const std::exception& e) {
                Logger::error(std::string("Failed to parse config.toml: ") + e.what());
                // Create a new empty data if it's corrupted
                data = toml::table{};
            }
        } else {
            data = toml::table{};
        }

        data.as_table()[key] = value;

        try {
            std::ofstream out(configPath, std::ios_base::out | std::ios_base::trunc);
            if (!out.is_open()) {
                Logger::error("Failed to open config.toml for writing");
                return false;
            }
            out << data;
            return true;
        } catch (const std::exception& e) {
            Logger::error(std::string("Failed to write to config.toml: ") + e.what());
            return false;
        }
    }

    bool removeSetting(const std::string& key) {
        std::string configPath = getConfigFilePath();
        if (!std::filesystem::exists(configPath)) {
            return true;
        }

        toml::value data;
        try {
            data = toml::parse(configPath);
        } catch (const std::exception& e) {
            Logger::error(std::string("Failed to parse config.toml: ") + e.what());
            return false;
        }

        if (data.as_table().count(key)) {
            data.as_table().erase(key);
            try {
                std::ofstream out(configPath, std::ios_base::out | std::ios_base::trunc);
                if (!out.is_open()) {
                    Logger::error("Failed to open config.toml for writing");
                    return false;
                }
                out << data;
                return true;
            } catch (const std::exception& e) {
                Logger::error(std::string("Failed to write to config.toml: ") + e.what());
                return false;
            }
        }
        return true;
    }
} // namespace AppConfig
