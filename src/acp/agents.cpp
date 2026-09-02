#include "acp/agents.hpp"
#include "acp/registry.hpp"
#include <algorithm>
#include <cstdlib>
#include <spdlog/spdlog.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace AcpAgents {

    // npm-style install commands for a package, in the order we prefer them
    std::vector<AcpInstallOption> npmInstalls(const std::string& package) {
        return {
            {"npm", "npm", "npm install -g " + package},
            {"bun", "bun", "bun add -g " + package},
            {"pnpm", "pnpm", "pnpm add -g " + package},
            {"yarn", "yarn", "yarn global add " + package},
        };
    }

    const std::vector<AcpAgentDef>& catalog() {
        static const std::vector<AcpAgentDef> defs = {
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

    const std::vector<AcpRunner>& runners() {
        // npx first: it is the most widely present and what the packages target.
        // bunx and pnpm dlx cover users who never installed npm.
        static const std::vector<AcpRunner> list = {
            {"npx", {"npx", "--yes"}, false}, {"bunx", {"bunx"}, false},
            {"pnpm", {"pnpm", "dlx"}, false}, {"yarn", {"yarn", "dlx"}, false},
            {"uvx", {"uvx"}, true},
        };
        return list;
    }

    std::vector<AcpAgentDef> availableAgents() {
        std::vector<AcpAgentDef> defs = catalog();
        for (const auto& installed : AcpRegistry::installedAgents()) {
            const bool known = std::any_of(defs.begin(), defs.end(), [&](const AcpAgentDef& d) {
                return d.id == installed.id;
            });
            if (known) {
                continue;
            }
            AcpAgentDef def;
            def.id = installed.id;
            def.name = installed.name;
            def.authHint = "Check the agent's own documentation for how to sign in.";
            defs.push_back(std::move(def)); // resolveInvocation finds the managed binary
        }
        return defs;
    }

    const AcpAgentDef* find(const std::string& id) {
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
            ProcessSpec spec;
            spec.args = {shell && *shell ? shell : "/bin/sh", "-l", "-c", "printf %s \"$PATH\""};
            const ProcessResult res = ProcessRunner::run(spec);
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

    std::optional<std::vector<std::string>> resolveInvocation(const AcpAgentDef& def) {
        // a binary we downloaded from the registry wins: it needs no runtime at all
        if (auto managed = AcpRegistry::installedCommand(def.id)) {
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

    const AcpInstallOption* resolveInstall(const AcpAgentDef& def) {
        for (const auto& option : def.installOptions) {
            if (executableExists(option.tool)) {
                return &option;
            }
        }
        return nullptr;
    }

} // namespace AcpAgents

void AcpAgentInstaller::start(const std::string& installCmd) {
    if (op_.isRunning()) {
        return;
    }
    result_ = {};
    op_.start([installCmd] {
        ProcessSpec spec;
        spec.args = {"/bin/sh", "-lc", installCmd};
        return ProcessRunner::run(spec);
    });
}

bool AcpAgentInstaller::isRunning() const {
    return op_.isRunning();
}

bool AcpAgentInstaller::check() {
    return op_.check([this](ProcessResult res) { result_ = std::move(res); });
}
