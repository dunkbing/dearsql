#pragma once

#include <string>
#include <utility>

struct SSHConfig;

// Manages a single SSH port-forward tunnel process.
// Each DatabaseInterface instance owns one.
class SSHTunnel {
public:
    SSHTunnel() = default;
    ~SSHTunnel();

    SSHTunnel(const SSHTunnel&) = delete;
    SSHTunnel& operator=(const SSHTunnel&) = delete;

    // Start tunnel. Blocks until local port is reachable or timeout.
    // If already running, returns success immediately.
    std::pair<bool, std::string> start(const SSHConfig& ssh, const std::string& remoteHost,
                                       int remotePort);

    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] int localPort() const {
        return localPort_;
    }

    // Original host/port before tunnel rewrote them
    [[nodiscard]] const std::string& remoteHost() const {
        return remoteHost_;
    }
    [[nodiscard]] int remotePort() const {
        return remotePort_;
    }
    [[nodiscard]] bool hasOriginals() const {
        return remotePort_ != 0;
    }

private:
    static int findAvailablePort();
    bool waitForPortReady(int port, int timeoutMs) const;
    std::string createAskPassScript(const std::string& secret) const;
    void cleanupAskPassScript();

    pid_t sshPid_ = -1;
    int localPort_ = 0;
    std::string remoteHost_;
    int remotePort_ = 0;
    std::string askPassPath_;
};
