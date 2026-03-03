#pragma once

#include <string>
#include <vector>

struct SSHHostEntry {
    std::string alias;        // Host line value
    std::string hostname;     // HostName directive
    std::string user;         // User directive
    int port = 22;            // Port directive
    std::string identityFile; // IdentityFile directive (~ expanded)
};

// Parse ~/.ssh/config (or given path). Skips wildcard-only entries.
std::vector<SSHHostEntry> parseSSHConfig(const std::string& path = "");
