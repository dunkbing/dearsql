#include "ai/acp_agents.hpp"
#include "ai/acp_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <spdlog/spdlog.h>

#if !defined(_WIN32)
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AcpAgents {

    namespace {
        // npm-style install commands for a package, in preference order
        std::vector<InstallOption> npmInstalls(const std::string& package) {
            return {
                {"npm", "npm", "npm install -g " + package},
                {"bun", "bun", "bun add -g " + package},
                {"pnpm", "pnpm", "pnpm add -g " + package},
                {"yarn", "yarn", "yarn global add " + package},
            };
        }

#if defined(_WIN32)
        std::wstring toWide(const std::string& value) {
            if (value.empty()) {
                return {};
            }
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                 static_cast<int>(value.size()), nullptr, 0);
            if (size <= 0) {
                return {};
            }
            std::wstring out(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), out.data(), size);
            return out;
        }

        std::string toUtf8(const std::wstring& value) {
            if (value.empty()) {
                return {};
            }
            const int size =
                WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                    nullptr, 0, nullptr, nullptr);
            if (size <= 0) {
                return {};
            }
            std::string out(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                out.data(), size, nullptr, nullptr);
            return out;
        }

        std::optional<std::string> findExecutable(const std::string& name) {
            const std::wstring wide = toWide(name);
            if (wide.empty() && !name.empty()) {
                return std::nullopt;
            }
            const std::array<const wchar_t*, 4> extensions = {nullptr, L".exe", L".cmd", L".bat"};
            std::array<wchar_t, 32768> path{};
            for (const wchar_t* extension : extensions) {
                const DWORD size =
                    SearchPathW(nullptr, wide.c_str(), extension, static_cast<DWORD>(path.size()),
                                path.data(), nullptr);
                if (size > 0 && size < path.size()) {
                    return toUtf8(std::wstring(path.data(), size));
                }
            }
            return std::nullopt;
        }
#endif
    } // namespace

    const std::vector<AgentDef>& catalog() {
        static const std::vector<AgentDef> defs = {
            {
                "claude-code",
                "Claude Code",
                {"claude-agent-acp"},
                "@agentclientprotocol/claude-agent-acp",
                "",
                {},
                npmInstalls("@agentclientprotocol/claude-agent-acp"),
                "Log in by running `claude /login` in a terminal, or set ANTHROPIC_API_KEY.",
            },
            {
                "gemini",
                "Gemini CLI",
                {"gemini", "--experimental-acp"},
                "@google/gemini-cli",
                "",
                {"--experimental-acp"},
                npmInstalls("@google/gemini-cli"),
                "Run `gemini` once in a terminal to sign in.",
            },
            {
                "codex",
                "Codex",
                {"codex-acp"},
                "@zed-industries/codex-acp",
                "",
                {},
                npmInstalls("@zed-industries/codex-acp"),
                "Sign in with `codex login` or set OPENAI_API_KEY.",
            },
        };
        return defs;
    }

    const std::vector<Runner>& runners() {
        // npx first: most widely present and what the packages target
        static const std::vector<Runner> list = {
            {"npx", {"npx", "--yes"}, false}, {"bunx", {"bunx"}, false},
            {"pnpm", {"pnpm", "dlx"}, false}, {"yarn", {"yarn", "dlx"}, false},
            {"uvx", {"uvx"}, true},
        };
        return list;
    }

    std::vector<AgentDef> availableAgents() {
        std::vector<AgentDef> defs = catalog();
        for (const auto& installed : AcpRegistry::installedAgents()) {
            const bool known = std::any_of(defs.begin(), defs.end(),
                                           [&](const AgentDef& d) { return d.id == installed.id; });
            if (known) {
                continue;
            }
            AgentDef def;
            def.id = installed.id;
            def.name = installed.name;
            def.authHint = "Check the agent's own documentation for how to sign in.";
            defs.push_back(std::move(def)); // resolveInvocation finds the managed binary
        }
        return defs;
    }

    const AgentDef* find(const std::string& id) {
        for (const auto& def : catalog()) {
            if (def.id == id) {
                return &def;
            }
        }
        return nullptr;
    }

    const std::string& loginShellPath() {
        static const std::string path = [] {
#if defined(_WIN32)
            const char* envPath = std::getenv("PATH");
            return std::string(envPath ? envPath : "");
#else
            const char* shell = std::getenv("SHELL");
            const ProcessResult res = ProcessRunner::run(
                {.args = {shell && *shell ? shell : "/bin/sh", "-l", "-c", "printf %s \"$PATH\""}});
            if (!res.success || res.output.empty()) {
                spdlog::warn("ACP: could not read login shell PATH: {}", res.errorMessage);
                const char* envPath = std::getenv("PATH");
                return std::string(envPath ? envPath : "");
            }
            // strip stray newlines some shells print on login
            std::string p = res.output;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) {
                p.pop_back();
            }
            if (const auto nl = p.find_last_of('\n'); nl != std::string::npos) {
                p = p.substr(nl + 1);
            }
            return p;
#endif
        }();
        return path;
    }

    bool executableExists(const std::string& name) {
#if defined(_WIN32)
        return findExecutable(name).has_value();
#else
        if (name.find('/') != std::string::npos) {
            return access(name.c_str(), X_OK) == 0;
        }
        const std::string& path = loginShellPath();
        size_t pos = 0;
        while (pos <= path.size()) {
            auto colon = path.find(':', pos);
            if (colon == std::string::npos) {
                colon = path.size();
            }
            const std::string dir = path.substr(pos, colon - pos);
            if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) {
                return true;
            }
            pos = colon + 1;
        }
        return false;
#endif
    }

    std::optional<std::vector<std::string>> resolveInvocation(const AgentDef& def) {
        // a registry binary wins: it needs no runtime at all
        if (auto managed = AcpRegistry::installedCommand(def.id)) {
            managed->insert(managed->end(), def.runArgs.begin(), def.runArgs.end());
            return managed;
        }
        if (!def.runCmd.empty()) {
#if defined(_WIN32)
            if (auto executable = findExecutable(def.runCmd.front())) {
                std::vector<std::string> argv = def.runCmd;
                argv.front() = std::move(*executable);
                return argv;
            }
#else
            if (executableExists(def.runCmd.front())) {
                return def.runCmd;
            }
#endif
        }
        // the managed bun beats runners on PATH: it was downloaded for this
        if (!def.npmPackage.empty()) {
            if (auto bun = AcpRegistry::bunPath()) {
                std::vector<std::string> argv = {*bun, "x", def.npmPackage};
                argv.insert(argv.end(), def.runArgs.begin(), def.runArgs.end());
                return argv;
            }
        }
        for (const auto& runner : runners()) {
            const std::string& package = runner.python ? def.pyPackage : def.npmPackage;
            if (package.empty()) {
                continue;
            }
#if defined(_WIN32)
            auto executable = findExecutable(runner.tool);
            if (!executable) {
                continue;
            }
#else
            if (!executableExists(runner.tool)) {
                continue;
            }
#endif
            std::vector<std::string> argv = runner.prefix;
#if defined(_WIN32)
            argv.front() = std::move(*executable);
#endif
            argv.push_back(package);
            argv.insert(argv.end(), def.runArgs.begin(), def.runArgs.end());
            return argv;
        }
        return std::nullopt;
    }

    const InstallOption* resolveInstall(const AgentDef& def) {
        for (const auto& option : def.installOptions) {
            if (executableExists(option.tool)) {
                return &option;
            }
        }
        return nullptr;
    }

    ProcessResult runInstall(const std::string& command) {
#if defined(_WIN32)
        return ProcessRunner::run({.args = {"cmd.exe", "/d", "/s", "/c", command}});
#else
        return ProcessRunner::run({.args = {"/bin/sh", "-lc", command}});
#endif
    }

} // namespace AcpAgents

void AcpAgentInstaller::start(const std::string& installCmd) {
    if (op_.isRunning()) {
        return;
    }
    result_ = {};
    op_.start([installCmd] { return AcpAgents::runInstall(installCmd); });
}

bool AcpAgentInstaller::isRunning() const {
    return op_.isRunning();
}

bool AcpAgentInstaller::check() {
    return op_.check([this](ProcessResult res) { result_ = std::move(res); });
}
