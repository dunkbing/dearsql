#pragma once

#include <memory>
#include <string>
#include <utility>

struct SSHConfig;

// a local port forwarded over ssh (libssh2, in process): every connection to
// 127.0.0.1:localPort() becomes a direct-tcpip channel to remoteHost:remotePort.
// each DatabaseInterface instance owns one
class SSHTunnel {
public:
    SSHTunnel(); // defined with Impl, in ssh_tunnel.cpp
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
    struct Impl; // the libssh2 session and the thread forwarding its channels
    std::unique_ptr<Impl> impl_;
    int localPort_ = 0;
    std::string remoteHost_;
    int remotePort_ = 0;
};
