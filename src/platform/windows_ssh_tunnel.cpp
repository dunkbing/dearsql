#if defined(_WIN32)

#include "database/db_interface.hpp"
#include "database/ssh_tunnel.hpp"
#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

// winsock init helper
static struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() {
        WSACleanup();
    }
} s_wsaInit;

SSHTunnel::~SSHTunnel() {
    stop();
}

std::pair<bool, std::string> SSHTunnel::start(const SSHConfig& ssh, const std::string& remoteHost,
                                              int remotePort) {
    if (isRunning())
        return {true, ""};

    remoteHost_ = remoteHost;
    remotePort_ = remotePort;

    localPort_ = findAvailablePort();
    if (localPort_ <= 0)
        return {false, "No available local port for SSH tunnel"};

    std::string forward = "127.0.0.1:" + std::to_string(localPort_) + ":" + remoteHost + ":" +
                          std::to_string(remotePort);

    // build command line
    std::string cmdline = "ssh -N";
    cmdline += " -o ExitOnForwardFailure=yes";

    const char* strictHostKeyChecking = std::getenv("DEARSQL_SSH_STRICT_HOST_KEY_CHECKING");
    cmdline += " -o StrictHostKeyChecking=";
    cmdline +=
        (strictHostKeyChecking && *strictHostKeyChecking) ? strictHostKeyChecking : "accept-new";

    if (const char* knownHostsFile = std::getenv("DEARSQL_SSH_KNOWN_HOSTS_FILE");
        knownHostsFile && *knownHostsFile) {
        cmdline += " -o UserKnownHostsFile=";
        cmdline += knownHostsFile;
    }

    cmdline += " -o ServerAliveInterval=30";
    cmdline += " -o ServerAliveCountMax=3";
    cmdline += " -o ConnectTimeout=10";
    cmdline += " -L " + forward;
    cmdline += " -p " + std::to_string(ssh.port);

    if (ssh.authMethod == SSHAuthMethod::PrivateKey) {
        if (ssh.privateKeyPath.empty())
            return {false, "Private key path is required"};

        std::string keyPath = ssh.privateKeyPath;
        if (keyPath.starts_with("~/")) {
            if (const char* home = std::getenv("USERPROFILE"))
                keyPath = std::string(home) + keyPath.substr(1);
        }

        if (!std::filesystem::exists(keyPath))
            return {false, "Private key not found: " + keyPath};

        cmdline += " -i \"" + keyPath + "\"";
        cmdline += " -o PubkeyAuthentication=yes";
        cmdline += " -o PasswordAuthentication=no";
        cmdline += " -o PreferredAuthentications=publickey";
    } else {
        cmdline += " -o PasswordAuthentication=yes";
        cmdline += " -o PreferredAuthentications=password";
        cmdline += " -o PubkeyAuthentication=no";
    }

    cmdline += " " + ssh.username + "@" + ssh.host;

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return {false, "Failed to spawn ssh: error code " + std::to_string(GetLastError())};
    }

    CloseHandle(pi.hThread);
    processHandle_ = pi.hProcess;

    spdlog::info("SSH tunnel spawned (pid {}) forwarding 127.0.0.1:{} -> {}:{} via {}",
                 pi.dwProcessId, localPort_, remoteHost, remotePort, ssh.host);

    const int failedLocalPort = localPort_;
    if (!waitForPortReady(failedLocalPort, 15000)) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &exitCode) &&
            exitCode != STILL_ACTIVE) {
            CloseHandle(static_cast<HANDLE>(processHandle_));
            processHandle_ = nullptr;
            if (exitCode == 255)
                return {false, "SSH connection failed (authentication, host key verification, or "
                               "forwarding setup error)"};
            return {false, "SSH process exited with code " + std::to_string(exitCode)};
        }
        stop();
        return {false, "SSH tunnel timed out waiting for port " + std::to_string(failedLocalPort)};
    }

    return {true, ""};
}

void SSHTunnel::stop() {
    if (processHandle_) {
        TerminateProcess(static_cast<HANDLE>(processHandle_), 0);
        WaitForSingleObject(static_cast<HANDLE>(processHandle_), 2000);
        CloseHandle(static_cast<HANDLE>(processHandle_));
        spdlog::info("SSH tunnel stopped");
        processHandle_ = nullptr;
    }
    localPort_ = 0;
}

bool SSHTunnel::isRunning() const {
    if (!processHandle_)
        return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &exitCode))
        return false;
    if (exitCode != STILL_ACTIVE) {
        CloseHandle(static_cast<HANDLE>(const_cast<SSHTunnel*>(this)->processHandle_));
        const_cast<SSHTunnel*>(this)->processHandle_ = nullptr;
        return false;
    }
    return true;
}

int SSHTunnel::findAvailablePort() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
        return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return -1;
    }

    int len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        closesocket(sock);
        return -1;
    }

    int port = ntohs(addr.sin_port);
    closesocket(sock);
    return port;
}

bool SSHTunnel::waitForPortReady(int port, int timeoutMs) const {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET)
            return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<u_short>(port));

        int rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closesocket(sock);
        if (rc == 0)
            return true;

        if (!isRunning())
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

#endif
