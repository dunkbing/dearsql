#pragma once

#include "database/async_helper.hpp"
#include <acp/registry.hpp>
#include <string>
#include <vector>

// Registry access lives in external/acp-cpp (built with ACP_REGISTRY); this is
// the background fetch + install wrapper the UI polls once per frame.
using AcpRegistryAgent = acp::registry::Agent;
namespace AcpRegistry = acp::registry;

class AcpRegistryClient {
public:
    void startFetch();
    void startInstall(const AcpRegistryAgent& agent);
    bool poll(); // true when an operation finished this call

    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] const std::vector<AcpRegistryAgent>& agents() const {
        return agents_;
    }
    [[nodiscard]] const std::string& error() const {
        return error_;
    }
    [[nodiscard]] const std::string& installedId() const {
        return installedId_;
    }

private:
    struct FetchResult {
        std::vector<AcpRegistryAgent> agents;
        std::string error;
    };
    struct InstallResult {
        std::string agentId;
        std::string error;
    };

    AsyncOperation<FetchResult> fetchOp_;
    AsyncOperation<InstallResult> installOp_;
    std::vector<AcpRegistryAgent> agents_;
    std::string error_;
    std::string installedId_;
};
