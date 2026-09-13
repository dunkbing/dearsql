#pragma once

#include "database/async_helper.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Everything DearSQL downloads into ~/.dearsql/agents so an agent can run:
// prebuilt binaries from the official ACP registry
// (https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json) and
// the managed Bun runtime for agents that only ship as npm packages. Blocking;
// AcpRegistryClient below is the background wrapper the panel polls.
namespace AcpRegistry {

    struct Agent {
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

    // "darwin-aarch64", "linux-x86_64", ... as keyed by distribution.binary
    std::string platformKey();

    std::filesystem::path installRoot(); // ~/.dearsql/agents
    std::filesystem::path installDir(const std::string& agentId);

    // resolved argv for an already-installed agent, or nullopt
    std::optional<std::vector<std::string>> installedCommand(const std::string& agentId);

    struct Installed {
        std::string id;
        std::string name;
    };
    std::vector<Installed> installedAgents();

    // both report failure through the error string
    std::vector<Agent> fetch(std::string& error);
    bool installBinary(const Agent& agent, std::string& error);

    // Managed Bun runtime: a pinned release from GitHub, verified against the
    // release's SHASUMS256.txt, kept in installRoot()/bun. AcpAgents runs
    // npm-only agents as `bun x <package>` through it ahead of PATH runners.
    constexpr const char* BUN_VERSION = "1.4.2";
    std::optional<std::string> bunPath(); // set when BUN_VERSION is installed
    bool installBun(std::string& error);
    // env for agent launches while the managed bun is installed: keeps its
    // package cache under installRoot()/bun instead of ~/.bun
    std::vector<std::pair<std::string, std::string>> bunEnv();

} // namespace AcpRegistry

using AcpRegistryAgent = AcpRegistry::Agent;

class AcpRegistryClient {
public:
    void startFetch();
    void startInstall(const AcpRegistryAgent& agent);
    void startInstallBun(); // managed runtime; installedId() reads "Bun" when done
    bool poll();            // true when an operation finished this call

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
