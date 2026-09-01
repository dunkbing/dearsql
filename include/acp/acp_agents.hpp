#pragma once

#include "database/async_helper.hpp"
#include "utils/process_runner.hpp"
#include <optional>
#include <string>
#include <vector>

// Built-in ACP agent catalog + install support. Modeled on Toad's agent store
// and the official ACP registry: each agent has a preferred binary invocation,
// an npx zero-install fallback, and a shell install command.

struct AcpAgentDef {
    std::string id;
    std::string name;
    std::vector<std::string> runCmd; // preferred (binary on PATH)
    std::vector<std::string> npxCmd; // fallback via npx, empty = none
    std::string installCmd;          // run with `sh -lc` by the Install button
    std::string authHint;            // shown when the agent reports auth errors
};

namespace AcpAgents {
    const std::vector<AcpAgentDef>& catalog();
    const AcpAgentDef* find(const std::string& id);

    // PATH of the user's login shell (cached); empty if it can't be determined
    const std::string& loginShellPath();

    // true if `name` resolves to an executable on the login-shell PATH
    bool executableExists(const std::string& name);

    // command line to launch the agent, or nullopt if neither the binary nor
    // npx is available (-> offer install)
    std::optional<std::vector<std::string>> resolveInvocation(const AcpAgentDef& def);
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
