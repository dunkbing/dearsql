#include "database/ssh_tunnel.hpp"
#include "database/db_interface.hpp"
#include "utils/logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

SSHTunnel::~SSHTunnel() {
    stop();
}

std::pair<bool, std::string> SSHTunnel::start(const SSHConfig& ssh, const std::string& remoteHost,
                                              int remotePort) {
    if (isRunning())
        return {true, ""};

    remoteHost_ = remoteHost;
    remotePort_ = remotePort;

    // Find a free local port
    localPort_ = findAvailablePort();
    if (localPort_ <= 0)
        return {false, "No available local port for SSH tunnel"};

    // Build the -L forward spec
    std::string forward = "127.0.0.1:" + std::to_string(localPort_) + ":" + remoteHost + ":" +
                          std::to_string(remotePort);

    // Build argv
    std::vector<std::string> args;
    args.push_back("ssh");
    args.push_back("-N"); // no remote command
    args.push_back("-o");
    args.push_back("ExitOnForwardFailure=yes");
    args.push_back("-o");
    args.push_back("StrictHostKeyChecking=accept-new");
    args.push_back("-o");
    args.push_back("ServerAliveInterval=30");
    args.push_back("-o");
    args.push_back("ServerAliveCountMax=3");
    args.push_back("-o");
    args.push_back("ConnectTimeout=10");
    args.push_back("-L");
    args.push_back(forward);
    args.push_back("-p");
    args.push_back(std::to_string(ssh.port));

    if (ssh.authMethod == SSHAuthMethod::PrivateKey) {
        if (ssh.privateKeyPath.empty())
            return {false, "Private key path is required"};

        // Expand ~ to home directory
        std::string keyPath = ssh.privateKeyPath;
        if (keyPath.starts_with("~/")) {
            if (const char* home = std::getenv("HOME"))
                keyPath = std::string(home) + keyPath.substr(1);
        }

        if (!std::filesystem::exists(keyPath))
            return {false, "Private key not found: " + keyPath};

        args.push_back("-i");
        args.push_back(keyPath);
        args.push_back("-o");
        args.push_back("PubkeyAuthentication=yes");
        args.push_back("-o");
        args.push_back("PasswordAuthentication=no");
        args.push_back("-o");
        args.push_back("PreferredAuthentications=publickey");
    } else {
        args.push_back("-o");
        args.push_back("PasswordAuthentication=yes");
        args.push_back("-o");
        args.push_back("PreferredAuthentications=password");
        args.push_back("-o");
        args.push_back("PubkeyAuthentication=no");
    }

    args.push_back(ssh.username + "@" + ssh.host);

    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // Build environment: inherit current env, add SSH_ASKPASS if needed
    std::vector<std::string> envStrings;
    std::vector<const char*> envp;
    bool needAskPass = (ssh.authMethod == SSHAuthMethod::Password && !ssh.password.empty());

    if (needAskPass) {
        askPassPath_ = createAskPassScript(ssh.password);
        if (askPassPath_.empty())
            return {false, "Failed to create SSH_ASKPASS helper"};

        // Copy current environment, replacing/adding SSH_ASKPASS vars
        for (char** e = environ; *e; ++e) {
            std::string var(*e);
            if (var.starts_with("SSH_ASKPASS=") || var.starts_with("SSH_ASKPASS_REQUIRE=") ||
                var.starts_with("DISPLAY="))
                continue;
            envStrings.push_back(var);
        }
        envStrings.push_back("SSH_ASKPASS=" + askPassPath_);
        envStrings.push_back("SSH_ASKPASS_REQUIRE=force");
        envStrings.push_back("DISPLAY=:0");
        for (const auto& s : envStrings)
            envp.push_back(s.c_str());
        envp.push_back(nullptr);
    }

    // Spawn ssh process
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Redirect stdout/stderr to /dev/null
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    // Detach stdin so ssh doesn't try to read from terminal
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    int rc = posix_spawnp(&sshPid_, "ssh", &actions, &attr, const_cast<char* const*>(argv.data()),
                          needAskPass ? const_cast<char* const*>(envp.data()) : environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        sshPid_ = -1;
        cleanupAskPassScript();
        return {false, "Failed to spawn ssh: " + std::string(std::strerror(rc))};
    }

    Logger::info("SSH tunnel spawned (pid " + std::to_string(sshPid_) +
                 ") forwarding 127.0.0.1:" + std::to_string(localPort_) + " -> " + remoteHost +
                 ":" + std::to_string(remotePort) + " via " + ssh.host);

    // Wait for the tunnel port to become reachable
    if (!waitForPortReady(localPort_, 15000)) {
        // Check if ssh process died
        int status = 0;
        pid_t result = waitpid(sshPid_, &status, WNOHANG);
        if (result == sshPid_) {
            sshPid_ = -1;
            cleanupAskPassScript();
            if (WIFEXITED(status) && WEXITSTATUS(status) == 255)
                return {false, "SSH authentication failed"};
            return {false, "SSH process exited with code " +
                               std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1)};
        }
        // Process still running but port not reachable — kill it
        stop();
        return {false, "SSH tunnel timed out waiting for port " + std::to_string(localPort_)};
    }

    cleanupAskPassScript();
    return {true, ""};
}

void SSHTunnel::stop() {
    if (sshPid_ > 0) {
        kill(sshPid_, SIGTERM);
        // Give it a moment to exit gracefully
        int status = 0;
        for (int i = 0; i < 10; ++i) {
            if (waitpid(sshPid_, &status, WNOHANG) != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Force kill if still alive
        if (kill(sshPid_, 0) == 0) {
            kill(sshPid_, SIGKILL);
            waitpid(sshPid_, &status, 0);
        }
        Logger::info("SSH tunnel stopped (pid " + std::to_string(sshPid_) + ")");
        sshPid_ = -1;
    }
    localPort_ = 0;
    cleanupAskPassScript();
}

bool SSHTunnel::isRunning() const {
    if (sshPid_ <= 0)
        return false;
    // Check if process is still alive without blocking
    int status = 0;
    pid_t result = waitpid(sshPid_, &status, WNOHANG);
    if (result == 0)
        return true; // still running
    // Process exited — clean up pid (const_cast is safe here; we're only resetting cached state)
    const_cast<SSHTunnel*>(this)->sshPid_ = -1;
    return false;
}

int SSHTunnel::findAvailablePort() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // let OS assign

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        close(sock);
        return -1;
    }

    int port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

bool SSHTunnel::waitForPortReady(int port, int timeoutMs) const {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));

        int rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        close(sock);
        if (rc == 0)
            return true;

        // Check if ssh process died
        if (sshPid_ > 0) {
            int status = 0;
            if (waitpid(sshPid_, &status, WNOHANG) != 0)
                return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

std::string SSHTunnel::createAskPassScript(const std::string& secret) const {
    // Escape single quotes in the secret for bash
    std::string escaped;
    for (char c : secret) {
        if (c == '\'')
            escaped += "'\\''";
        else
            escaped += c;
    }

    char tmpl[] = "/tmp/ssh_askpass_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return "";

    std::string content = "#!/bin/sh\necho '" + escaped + "'\n";
    auto written = write(fd, content.c_str(), content.size());
    close(fd);

    if (written < 0 || static_cast<size_t>(written) != content.size()) {
        unlink(tmpl);
        return "";
    }

    chmod(tmpl, 0700);
    return tmpl;
}

void SSHTunnel::cleanupAskPassScript() {
    if (!askPassPath_.empty()) {
        unlink(askPassPath_.c_str());
        askPassPath_.clear();
    }
}
