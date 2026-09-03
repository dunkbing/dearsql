#include "acp/agents.hpp"
#include "acp/log.hpp"
#include "acp/registry.hpp"

#include <algorithm>
#include <cstdlib>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace acp::agents {

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
        for (const auto& installed : registry::installedAgents()) {
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
            return std::string{};
#else
            const char* shell = std::getenv("SHELL");
            const RunResult res =
                run({shell && *shell ? shell : "/bin/sh", "-l", "-c", "printf %s \"$PATH\""});
            if (!res.ok || res.output.empty()) {
                log(LogLevel::Warn, "could not read login shell PATH: " + res.error);
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
        return false;
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
        if (auto managed = registry::installedCommand(def.id)) {
            managed->insert(managed->end(), def.runArgs.begin(), def.runArgs.end());
            return managed;
        }
        if (!def.runCmd.empty() && executableExists(def.runCmd.front())) {
            return def.runCmd;
        }
        for (const auto& runner : runners()) {
            const std::string& package = runner.python ? def.pyPackage : def.npmPackage;
            if (package.empty() || !executableExists(runner.tool)) {
                continue;
            }
            std::vector<std::string> argv = runner.prefix;
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

    RunResult runInstall(const std::string& command) {
        return run({"/bin/sh", "-lc", command});
    }

} // namespace acp::agents
