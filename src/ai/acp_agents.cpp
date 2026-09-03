#include "ai/acp_agents.hpp"

void AcpAgentInstaller::start(const std::string& installCmd) {
    if (op_.isRunning()) {
        return;
    }
    result_ = {};
    op_.start([installCmd] { return acp::agents::runInstall(installCmd); });
}

bool AcpAgentInstaller::isRunning() const {
    return op_.isRunning();
}

bool AcpAgentInstaller::check() {
    return op_.check([this](acp::RunResult res) { result_ = std::move(res); });
}
