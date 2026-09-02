#pragma once

#include "database/async_helper.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Client for the official ACP agent registry
// (https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json).
//
// The registry lists each agent's distribution: an npm package for npx/bunx, a
// python one for uvx, and/or prebuilt binaries per platform. The binaries are
// the only route that needs no JavaScript runtime at all, so those are what we
// download and manage under ~/.dearsql/agents/<id>/.

struct AcpRegistryAgent {
    std::string id;
    std::string name;
    std::string description;
    std::string version;

    std::string npmPackage; // from distribution.npx
    std::string pyPackage;  // from distribution.uvx

    // distribution.binary entry matching this platform, when present
    bool hasBinary = false;
    std::string archiveUrl;
    std::string archiveSha256;
    std::string binaryCmd; // relative command inside the archive, e.g. "./amp-acp"
};

namespace AcpRegistry {

    // "darwin-aarch64", "linux-x86_64", ... as keyed by distribution.binary
    std::string platformKey();

    // where a downloaded agent lives
    std::filesystem::path installDir(const std::string& agentId);

    // resolved command for an already-installed agent, or nullopt
    std::optional<std::vector<std::string>> installedCommand(const std::string& agentId);

    // ids and display names of everything under ~/.dearsql/agents
    struct Installed {
        std::string id;
        std::string name;
    };
    std::vector<Installed> installedAgents();

    // blocking; both report failure through the error string
    std::vector<AcpRegistryAgent> fetch(std::string& error);
    bool installBinary(const AcpRegistryAgent& agent, std::string& error);

} // namespace AcpRegistry

// Background registry fetch + binary install, polled from the UI thread.
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
