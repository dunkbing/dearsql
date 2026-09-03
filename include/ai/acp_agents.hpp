#pragma once

#include "database/async_helper.hpp"
#include <acp/agents.hpp>

// Agent catalog and launch resolution live in external/acp-cpp; these names keep
// the UI code reading naturally.
using AcpRunner = acp::agents::Runner;
using AcpInstallOption = acp::agents::InstallOption;
using AcpAgentDef = acp::agents::AgentDef;
namespace AcpAgents = acp::agents;

// Runs an agent's install command in the background, capturing output.
class AcpAgentInstaller {
public:
    void start(const std::string& installCmd);
    [[nodiscard]] bool isRunning() const;
    // poll; returns true when an install finished this call
    bool check();
    [[nodiscard]] const acp::RunResult& lastResult() const {
        return result_;
    }

private:
    AsyncOperation<acp::RunResult> op_;
    acp::RunResult result_;
};
