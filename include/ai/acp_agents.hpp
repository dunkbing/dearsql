#pragma once

#include "database/async_helper.hpp"
#include "utils/process_runner.hpp"
#include <optional>
#include <string>
#include <vector>

// The agent picker's catalog and how each entry gets launched. acp-cpp stays a
// protocol library (like the official Rust crate): which agents exist, where to
// find them on this machine, how to install them, is DearSQL's business.
namespace AcpAgents {

    // package runner that executes without installing first, in preference order
    struct Runner {
        std::string tool;                // executable that must exist, e.g. "bunx"
        std::vector<std::string> prefix; // argv prefix, e.g. {"npx", "--yes"}
        bool python = false;             // consumes pyPackage instead of npmPackage
    };

    // one way to install an agent for real, used when no runner is available
    struct InstallOption {
        std::string tool;    // must exist on PATH for this option to be offered
        std::string label;   // e.g. "npm"
        std::string command; // run with `sh -lc`
    };

    struct AgentDef {
        std::string id;
        std::string name;
        std::vector<std::string> runCmd;           // preferred: binary already on PATH
        std::string npmPackage;                    // for npx / bunx / pnpm dlx, empty = none
        std::string pyPackage;                     // for uvx, empty = none
        std::vector<std::string> runArgs;          // appended after the package
        std::vector<InstallOption> installOptions; // tried in order
        std::string authHint;                      // shown when the agent reports auth errors
        std::string registryId; // prebuilt binary in the ACP registry, downloaded on demand
    };

    const std::vector<AgentDef>& catalog();
    const AgentDef* find(const std::string& id);

    const std::vector<Runner>& runners();

    // PATH of the user's login shell (cached); GUI apps inherit a minimal one
    const std::string& loginShellPath();
    bool executableExists(const std::string& name);

    // argv to launch the agent: a registry binary, the binary on PATH, the managed
    // bun, else the first available runner. nullopt when nothing can run it
    std::optional<std::vector<std::string>> resolveInvocation(const AgentDef& def);

    // first install option whose tool is present, or nullptr
    const InstallOption* resolveInstall(const AgentDef& def);

    // run an install command through the login shell, blocking
    ProcessResult runInstall(const std::string& command);

} // namespace AcpAgents

using AcpRunner = AcpAgents::Runner;
using AcpInstallOption = AcpAgents::InstallOption;
using AcpAgentDef = AcpAgents::AgentDef;

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
