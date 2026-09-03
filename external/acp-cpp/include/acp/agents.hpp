#pragma once

#include "acp/process.hpp"
#include <optional>
#include <string>
#include <vector>

// Built-in agent catalog plus launch resolution. Agents are distributed as a
// binary on PATH, an npm package (npx/bunx/pnpm dlx/yarn dlx) or a python one
// (uvx), mirroring the official registry.
namespace acp::agents {

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
    };

    const std::vector<AgentDef>& catalog();

    // catalog plus agents downloaded from the registry (see registry.hpp)
    std::vector<AgentDef> availableAgents();
    const AgentDef* find(const std::string& id);

    const std::vector<Runner>& runners();

    // PATH of the user's login shell (cached); GUI apps inherit a minimal one
    const std::string& loginShellPath();
    bool executableExists(const std::string& name);

    // argv to launch the agent: a registry binary, the binary on PATH, else the
    // first available runner. nullopt when nothing can run it (offer an install)
    std::optional<std::vector<std::string>> resolveInvocation(const AgentDef& def);

    // first install option whose tool is present, or nullptr
    const InstallOption* resolveInstall(const AgentDef& def);

    // run an install command through the login shell, blocking
    RunResult runInstall(const std::string& command);

} // namespace acp::agents
