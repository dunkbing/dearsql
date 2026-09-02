#pragma once

#include "database/async_helper.hpp"
#include "utils/process_runner.hpp"
#include <optional>
#include <string>
#include <vector>

// Built-in ACP agent catalog + install support. Modeled on Toad's agent store
// and the official ACP registry, which distributes agents as a binary, an npm
// package (npx/bunx/pnpm dlx) or a python one (uvx).

// A package runner that executes without installing first. Ordered by
// preference in RUNNERS; the first one present on PATH wins.
struct AcpRunner {
    std::string tool;                // executable that must exist, e.g. "bunx"
    std::vector<std::string> prefix; // argv prefix, e.g. {"npx", "--yes"}
    bool python = false;             // consumes pyPackage instead of npmPackage
};

// One way to install an agent for real, used when no runner is available.
struct AcpInstallOption {
    std::string tool;    // must exist on PATH for this option to be offered
    std::string label;   // shown on the button, e.g. "npm"
    std::string command; // run with `sh -lc`
};

struct AcpAgentDef {
    std::string id;
    std::string name;
    std::vector<std::string> runCmd;              // preferred: binary already on PATH
    std::string npmPackage;                       // for npx / bunx / pnpm dlx, empty = none
    std::string pyPackage;                        // for uvx, empty = none
    std::vector<std::string> runArgs;             // args appended after the package
    std::vector<AcpInstallOption> installOptions; // tried in order
    std::string authHint;                         // shown when the agent reports auth errors
};

namespace AcpAgents {
    const std::vector<AcpAgentDef>& catalog();

    // built-in catalog plus any agent downloaded from the registry, which run
    // straight from ~/.dearsql/agents with no JavaScript runtime involved
    std::vector<AcpAgentDef> availableAgents();
    const AcpAgentDef* find(const std::string& id);

    // zero-install package runners, in preference order
    const std::vector<AcpRunner>& runners();

    // PATH of the user's login shell (cached); empty if it can't be determined
    const std::string& loginShellPath();

    // true if `name` resolves to an executable on the login-shell PATH
    bool executableExists(const std::string& name);

    // command line to launch the agent: the binary if present, else the first
    // available runner. nullopt when nothing can run it (-> offer install)
    std::optional<std::vector<std::string>> resolveInvocation(const AcpAgentDef& def);

    // first install option whose tool is present, or nullptr when none apply
    const AcpInstallOption* resolveInstall(const AcpAgentDef& def);
} // namespace AcpAgents

// Runs an agent's install command in the background, capturing output.
class AcpAgentInstaller {
public:
    void start(const std::string& installCmd);
    [[nodiscard]] bool isRunning() const;
    // poll; returns true when an install finished this call
    bool check();
    [[nodiscard]] const ProcessResult& lastResult() const {
        return result_;
    }

private:
    AsyncOperation<ProcessResult> op_;
    ProcessResult result_;
};
