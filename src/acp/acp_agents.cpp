#include "acp/acp_agents.hpp"
#include <cstdlib>
#include <spdlog/spdlog.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace AcpAgents {

    const std::vector<AcpAgentDef>& catalog() {
        static const std::vector<AcpAgentDef> defs = {
            {
                "claude-code",
                "Claude Code",
                {"claude-agent-acp"},
                {"npx", "--yes", "@agentclientprotocol/claude-agent-acp"},
                "npm install -g @agentclientprotocol/claude-agent-acp",
                "Log in by running `claude /login` in a terminal, or set ANTHROPIC_API_KEY.",
            },
            {
                "gemini",
                "Gemini CLI",
                {"gemini", "--experimental-acp"},
                {"npx", "--yes", "@google/gemini-cli", "--experimental-acp"},
                "npm install -g @google/gemini-cli",
                "Run `gemini` once in a terminal to sign in.",
            },
            {
                "codex",
                "Codex",
                {"codex-acp"},
                {"npx", "--yes", "@zed-industries/codex-acp"},
                "npm install -g @zed-industries/codex-acp",
                "Sign in with `codex login` or set OPENAI_API_KEY.",
            },
        };
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
        if (!def.runCmd.empty() && executableExists(def.runCmd.front())) {
            return def.runCmd;
        }
        if (!def.npxCmd.empty() && executableExists("npx")) {
            return def.npxCmd;
        }
        return std::nullopt;
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
