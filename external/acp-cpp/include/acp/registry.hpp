#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The official agent registry
// (https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json) lists
// each agent's distribution: an npm package, a python package and/or prebuilt
// binaries per platform. Binaries are downloaded to installRoot()/<id>/ and
// need no JavaScript runtime. fetch() and installBinary() require the library
// to be built with ACP_REGISTRY (httplib + OpenSSL).
namespace acp::registry {

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

    // where downloaded agents live; default ~/.acp/agents
    void setInstallRoot(std::filesystem::path root);
    std::filesystem::path installRoot();
    std::filesystem::path installDir(const std::string& agentId);

    // resolved argv for an already-installed agent, or nullopt
    std::optional<std::vector<std::string>> installedCommand(const std::string& agentId);

    struct Installed {
        std::string id;
        std::string name;
    };
    std::vector<Installed> installedAgents();

    // blocking; both report failure through the error string
    std::vector<Agent> fetch(std::string& error);
    bool installBinary(const Agent& agent, std::string& error);

} // namespace acp::registry
