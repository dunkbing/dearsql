#include "database/ssh_config_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

static std::string expandTilde(const std::string& path) {
    if (path.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"))
            return std::string(home) + path.substr(1);
    }
    return path;
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool isWildcard(const std::string& pattern) {
    return pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos;
}

std::vector<SSHHostEntry> parseSSHConfig(const std::string& path) {
    std::string configPath = path.empty() ? expandTilde("~/.ssh/config") : path;

    std::ifstream file(configPath);
    if (!file.is_open())
        return {};

    std::vector<SSHHostEntry> entries;
    SSHHostEntry current;
    bool inBlock = false;

    auto flushBlock = [&]() {
        if (inBlock && !current.alias.empty() && !isWildcard(current.alias)) {
            current.identityFile = expandTilde(current.identityFile);
            entries.push_back(std::move(current));
        }
        current = SSHHostEntry{};
        inBlock = false;
    };

    std::string line;
    while (std::getline(file, line)) {
        // Trim leading whitespace
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        line = line.substr(start);

        // Split into key and value
        auto sep = line.find_first_of(" \t");
        if (sep == std::string::npos)
            continue;

        std::string key = toLower(line.substr(0, sep));
        auto valStart = line.find_first_not_of(" \t", sep);
        if (valStart == std::string::npos)
            continue;
        std::string value = line.substr(valStart);

        if (key == "host") {
            flushBlock();
            current.alias = value;
            inBlock = true;
        } else if (key == "hostname") {
            current.hostname = value;
        } else if (key == "user") {
            current.user = value;
        } else if (key == "port") {
            current.port = std::atoi(value.c_str());
        } else if (key == "identityfile") {
            current.identityFile = value;
        }
    }

    flushBlock();
    return entries;
}
