#include "ai/acp_agents.hpp"
#include "ai/acp_registry.hpp"
#include "utils/process_runner.hpp"

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
                {},
                "@agentclientprotocol/claude-agent-acp",
                {},
                "Log in by running `claude /login` in a terminal, or set ANTHROPIC_API_KEY.",
            },
            {
                "gemini",
                "Gemini CLI",
                {},
                "@google/gemini-cli",
                {"--experimental-acp"},
                "Run `gemini` once in a terminal to sign in.",
            },
            {
                "codex",
                "Codex",
                {},
                "@zed-industries/codex-acp",
                {},
                "Sign in with `codex login` or set OPENAI_API_KEY.",
            },
            // binary-only: the registry archive already carries its args ("acp")
            {
                "cursor",
                "Cursor",
                {"cursor-agent", "acp"},
                "",
                {},
                "Run `cursor-agent login` in a terminal.",
                "cursor",
            },
            {
                "antigravity-acp",
                "Google Antigravity",
                {},
                "",
                {},
                "Sign in with your Google account when Antigravity asks.",
                "antigravity-acp",
            },
        };
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
            // a hung shell profile must not block agent lookup forever
            const ProcessResult res = ProcessRunner::run(
                {.args = {shell && *shell ? shell : "/bin/sh", "-l", "-c", "printf %s \"$PATH\""},
                 .timeout = std::chrono::seconds(5)});
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

    // the full path: posix_spawnp searches the app's own PATH, which is minimal when
    // DearSQL is opened from Finder or the Dock, not the login PATH the agent is given
    std::optional<std::string> locateExecutable(const std::string& name) {
#if defined(_WIN32)
        return findExecutable(name);
#else
        if (name.find('/') != std::string::npos) {
            return access(name.c_str(), X_OK) == 0 ? std::optional(name) : std::nullopt;
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
                return dir + "/" + name;
            }
            pos = colon + 1;
        }
        return std::nullopt;
#endif
    }

    std::optional<std::vector<std::string>> resolveInvocation(const AgentDef& def) {
        // a registry binary wins: it needs no runtime at all
        if (auto managed = AcpRegistry::installedCommand(def.id)) {
            managed->insert(managed->end(), def.runArgs.begin(), def.runArgs.end());
            return managed;
        }
        if (!def.npmPackage.empty()) {
            // the user's own bun first; ours only when there is none
            auto bun = locateExecutable("bun");
            if (!bun)
                bun = AcpRegistry::bunPath();
            if (!bun)
                return std::nullopt;
            std::vector<std::string> argv = {*bun, "x", def.npmPackage};
            argv.insert(argv.end(), def.runArgs.begin(), def.runArgs.end());
            return argv;
        }
        if (!def.runCmd.empty()) {
            if (auto executable = locateExecutable(def.runCmd.front())) {
                std::vector<std::string> argv = def.runCmd;
                argv.front() = std::move(*executable);
                return argv;
            }
        }
        return std::nullopt;
    }

} // namespace AcpAgents
