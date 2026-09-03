#include "acp/process.hpp"

#if defined(_WIN32)

namespace acp {
    RunResult run(const std::vector<std::string>&) {
        RunResult r;
        r.error = "process spawning is not supported on Windows yet";
        return r;
    }
} // namespace acp

#else

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace acp {

    RunResult run(const std::vector<std::string>& argv) {
        RunResult result;
        if (argv.empty()) {
            result.error = "empty command";
            return result;
        }

        int out[2];
        if (pipe(out) != 0) {
            result.error = std::strerror(errno);
            return result;
        }

        std::vector<const char*> cargv;
        for (const auto& a : argv) {
            cargv.push_back(a.c_str());
        }
        cargv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, out[0]);

        pid_t pid = -1;
        const int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr,
                                    const_cast<char* const*>(cargv.data()), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        if (rc != 0) {
            close(out[0]);
            result.error = std::strerror(rc);
            return result;
        }

        char chunk[4096];
        for (;;) {
            const ssize_t n = read(out[0], chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            result.output.append(chunk, static_cast<size_t>(n));
        }
        close(out[0]);

        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        result.ok = WIFEXITED(status);
        result.exitCode = result.ok ? WEXITSTATUS(status) : -1;
        return result;
    }

} // namespace acp

#endif
