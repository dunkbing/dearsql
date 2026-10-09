#include "utils/process_runner.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
#endif

namespace {
#if !defined(_WIN32)
    // fds must not leak into children spawned on other threads (ssh tunnel, acp agent),
    // or this pipe never reaches EOF while they live
    bool makeCloexecPipe(int fds[2]) {
#if defined(__linux__)
        return pipe2(fds, O_CLOEXEC) == 0;
#else
        // ponytail: macOS has no pipe2; a fork on another thread between pipe() and
        // fcntl() can still inherit these, POSIX_SPAWN_CLOEXEC_DEFAULT covers our spawns
        if (pipe(fds) != 0) {
            return false;
        }
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        return true;
#endif
    }

    // SIGTERM the child's group, SIGKILL after a 1s grace, then reap
    void terminateGroup(const pid_t pid, int& status) {
        kill(-pid, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            siginfo_t info{};
            // WNOWAIT keeps the zombie, so the group id cannot be reused before SIGKILL
            if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                info.si_pid == pid) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(-pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
#endif

#if defined(_WIN32)
    std::string quoteWindowsArg(const std::string& arg) {
        if (arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos) {
            std::string quoted = "\"";
            size_t backslashes = 0;
            for (const char c : arg) {
                if (c == '\\') {
                    ++backslashes;
                } else if (c == '"') {
                    quoted.append(backslashes * 2 + 1, '\\');
                    quoted += c;
                    backslashes = 0;
                } else {
                    quoted.append(backslashes, '\\');
                    backslashes = 0;
                    quoted += c;
                }
            }
            quoted.append(backslashes * 2, '\\');
            quoted += '"';
            return quoted;
        }
        return arg;
    }

    std::string buildCommandLine(const std::vector<std::string>& args) {
        std::string commandLine;
        for (const auto& arg : args) {
            if (!commandLine.empty())
                commandLine += ' ';
            commandLine += quoteWindowsArg(arg);
        }
        return commandLine;
    }

    std::vector<char>
    buildEnvironmentBlock(const std::unordered_map<std::string, std::string>& overrides) {
        std::vector<std::string> entries;
        LPCH env = GetEnvironmentStringsA();
        if (env) {
            for (LPCH current = env; *current != '\0'; current += std::strlen(current) + 1) {
                std::string entry = current;
                const auto eq = entry.find('=');
                const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
                if (!overrides.contains(key)) {
                    entries.push_back(std::move(entry));
                }
            }
            FreeEnvironmentStringsA(env);
        }

        for (const auto& [key, value] : overrides) {
            entries.push_back(key + "=" + value);
        }
        std::ranges::sort(entries);

        std::vector<char> block;
        for (const auto& entry : entries) {
            block.insert(block.end(), entry.begin(), entry.end());
            block.push_back('\0');
        }
        block.push_back('\0');
        return block;
    }
#endif
} // namespace

ProcessResult ProcessRunner::run(const ProcessSpec& spec, std::stop_token stop) {
    ProcessResult result;
    if (spec.args.empty() || spec.args.front().empty()) {
        result.errorMessage = "No executable specified";
        return result;
    }

#if defined(_WIN32)
    // ponytail: no cancel or timeout on windows yet, ReadFile blocks until exit
    (void)stop;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        result.errorMessage = "Failed to create process pipe";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string commandLine = buildCommandLine(spec.args);
    auto environmentBlock = buildEnvironmentBlock(spec.environment);

    const BOOL created =
        CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       environmentBlock.data(), nullptr, &si, &pi);
    CloseHandle(writePipe);

    if (!created) {
        CloseHandle(readPipe);
        result.errorMessage = std::format("Failed to start '{}'", spec.args.front());
        return result;
    }

    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        result.output.append(buffer, bytesRead);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.exitCode = static_cast<int>(exitCode);
    result.success = exitCode == 0;
    if (!result.success && result.output.empty()) {
        result.errorMessage =
            std::format("'{}' exited with code {}", spec.args.front(), result.exitCode);
    }
    return result;
#else
    int pipefd[2];
    if (!makeCloexecPipe(pipefd)) {
        result.errorMessage =
            std::format("Failed to create process pipe: {}", std::strerror(errno));
        return result;
    }

    // inherited environment plus overrides, built before the spawn
    std::vector<std::string> envStrings;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        if (!spec.environment.contains(std::string(entry.substr(0, eq)))) {
            envStrings.emplace_back(entry);
        }
    }
    for (const auto& [key, value] : spec.environment) {
        envStrings.push_back(key + "=" + value);
    }
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (auto& entry : envStrings) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    std::vector<char*> argv;
    argv.reserve(spec.args.size() + 1);
    for (const auto& arg : spec.args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // own process group so a cancel reaches the whole tree (sh -lc npm ...)
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    // only the fds named above survive into the child
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    sigset_t noSignals;
    sigemptyset(&noSignals);
    posix_spawnattr_setsigmask(&attr, &noSignals);
    sigset_t defaultSignals;
    sigemptyset(&defaultSignals);
    sigaddset(&defaultSignals, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaultSignals);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv.front(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);

    if (rc != 0) {
        close(pipefd[0]);
        result.exitCode = rc == ENOENT ? 127 : 126;
        result.errorMessage =
            rc == ENOENT
                ? std::format("'{}' was not found in PATH", spec.args.front())
                : std::format("Failed to start '{}': {}", spec.args.front(), std::strerror(rc));
        return result;
    }

    const auto deadline = spec.timeout.count() > 0 ? std::chrono::steady_clock::now() + spec.timeout
                                                   : std::chrono::steady_clock::time_point::max();
    const auto shouldStop = [&] {
        return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline;
    };

    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
    bool stopped = false;
    char buffer[4096];
    while (true) {
        if (shouldStop()) {
            stopped = true;
            break;
        }
        pollfd pfd{pipefd[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, 100);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.output.append(buffer, static_cast<size_t>(n));
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            break;
        }
    }
    close(pipefd[0]);

    // the pipe can close before the child exits, so the wait honours the token too
    int status = 0;
    bool reaped = false;
    while (!stopped) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) {
            reaped = w == pid;
            break;
        }
        if (shouldStop()) {
            stopped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stopped) {
        terminateGroup(pid, status);
        reaped = true;
    }

    if (stopped) {
        result.cancelled = true;
        result.exitCode = WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                          : WIFEXITED(status) ? WEXITSTATUS(status)
                                              : -1;
        result.errorMessage = stop.stop_requested()
                                  ? std::format("'{}' was cancelled", spec.args.front())
                                  : std::format("'{}' timed out after {} ms", spec.args.front(),
                                                spec.timeout.count());
        return result;
    }
    if (reaped && WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        result.success = result.exitCode == 0;
    } else if (reaped && WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
        result.success = false;
    }

    if (!result.success && result.output.empty()) {
        result.errorMessage =
            std::format("'{}' exited with code {}", spec.args.front(), result.exitCode);
    }
    return result;
#endif
}
