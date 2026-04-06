#pragma once

#include <string>

namespace AppConfig {
    // Reads a setting from the config.toml file.
    std::string getSetting(const std::string& key, const std::string& defaultValue = "");

    // Writes a setting to the config.toml file.
    bool setSetting(const std::string& key, const std::string& value);

    // Removes a setting from the config.toml file.
    bool removeSetting(const std::string& key);
} // namespace AppConfig
