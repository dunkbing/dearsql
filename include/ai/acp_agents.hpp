#pragma once

#include <optional>
#include <string>
#include <vector>

// The agent picker's catalog and how each entry gets launched. acp-cpp stays a
// protocol library (like the official Rust crate): which agents exist, where to
// find them on this machine, how to install them, is DearSQL's business.
namespace AcpAgents {

    struct AgentDef {
        std::string id;
        std::string name;
        std::vector<std::string> runCmd;  // a binary-only agent's command on PATH
        std::string npmPackage;           // run with the managed bun, empty = none
        std::vector<std::string> runArgs; // appended after the package
        std::string authHint;             // shown when the agent reports auth errors
        std::string registryId; // prebuilt binary in the ACP registry, downloaded on demand
    };

    const std::vector<AgentDef>& catalog();
    const AgentDef* find(const std::string& id);

    // PATH of the user's login shell (cached); GUI apps inherit a minimal one
    const std::string& loginShellPath();
    // full path of a command on the login PATH (or a path given as is)
    std::optional<std::string> locateExecutable(const std::string& name);

    // argv to launch the agent: a registry binary, else for npm agents `bun x` on the
    // user's bun or else the managed one (never the user's node, npx or a global install: the
    // packages need node 22+ and a version manager's is often older), else the binary on PATH;
    // argv[0] is always a full path. nullopt until the binary or bun is downloaded
    std::optional<std::vector<std::string>> resolveInvocation(const AgentDef& def);

} // namespace AcpAgents

using AcpAgentDef = AcpAgents::AgentDef;
