#include "ai/acp_registry.hpp"

void AcpRegistryClient::startFetch() {
    if (fetchOp_.isRunning()) {
        return;
    }
    error_.clear();
    fetchOp_.start([] {
        FetchResult result;
        result.agents = acp::registry::fetch(result.error);
        return result;
    });
}

void AcpRegistryClient::startInstall(const AcpRegistryAgent& agent) {
    if (installOp_.isRunning()) {
        return;
    }
    error_.clear();
    installedId_.clear();
    installOp_.start([agent] {
        InstallResult result;
        if (acp::registry::installBinary(agent, result.error)) {
            result.agentId = agent.id;
        }
        return result;
    });
}

bool AcpRegistryClient::poll() {
    bool finished = false;
    finished |= fetchOp_.check([this](FetchResult result) {
        agents_ = std::move(result.agents);
        error_ = std::move(result.error);
    });
    finished |= installOp_.check([this](InstallResult result) {
        installedId_ = std::move(result.agentId);
        error_ = std::move(result.error);
    });
    return finished;
}

bool AcpRegistryClient::isBusy() const {
    return fetchOp_.isRunning() || installOp_.isRunning();
}
