#include "acp/registry.hpp"

#include "utils/app_paths.hpp"
#include "utils/process_runner.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    constexpr const char* REGISTRY_HOST = "https://cdn.agentclientprotocol.com";
    constexpr const char* REGISTRY_PATH = "/registry/v1/latest/registry.json";

    std::string sha256Hex(const std::string& data) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            return "";
        }
        std::string out;
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
            EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
            EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
            static constexpr char HEX[] = "0123456789abcdef";
            out.reserve(len * 2);
            for (unsigned int i = 0; i < len; ++i) {
                out += HEX[digest[i] >> 4];
                out += HEX[digest[i] & 0x0F];
            }
        }
        EVP_MD_CTX_free(ctx);
        return out;
    }

    // split "https://host/a/b" into {"https://host", "/a/b"}
    std::pair<std::string, std::string> splitUrl(const std::string& url) {
        const auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos) {
            return {"", url};
        }
        const auto pathStart = url.find('/', schemeEnd + 3);
        if (pathStart == std::string::npos) {
            return {url, "/"};
        }
        return {url.substr(0, pathStart), url.substr(pathStart)};
    }

    std::string httpGet(const std::string& url, std::string& error) {
        const auto [host, path] = splitUrl(url);
        if (host.empty()) {
            error = "bad url: " + url;
            return "";
        }
        httplib::Client client(host);
        client.set_follow_location(true); // release assets redirect to a CDN
        client.set_connection_timeout(15);
        client.set_read_timeout(120);

        auto res = client.Get(path);
        if (!res) {
            error = "download failed: " + url;
            return "";
        }
        if (res->status != 200) {
            error = "http " + std::to_string(res->status) + " for " + url;
            return "";
        }
        return res->body;
    }
} // namespace

namespace AcpRegistry {

    std::string platformKey() {
#if defined(_WIN32)
        return "windows-x86_64";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
        return "darwin-aarch64";
#else
        return "darwin-x86_64";
#endif
#else
#if defined(__aarch64__)
        return "linux-aarch64";
#else
        return "linux-x86_64";
#endif
#endif
    }

    fs::path installDir(const std::string& agentId) {
        return AppPaths::dataDir() / "agents" / agentId;
    }

    std::optional<std::vector<std::string>> installedCommand(const std::string& agentId) {
        const fs::path marker = installDir(agentId) / ".cmd";
        std::ifstream in(marker);
        if (!in) {
            return std::nullopt;
        }
        std::string cmd;
        std::getline(in, cmd);
        if (cmd.empty()) {
            return std::nullopt;
        }
        const fs::path exe = installDir(agentId) / cmd;
        std::error_code ec;
        if (!fs::exists(exe, ec)) {
            return std::nullopt;
        }
        return std::vector<std::string>{exe.string()};
    }

    std::vector<Installed> installedAgents() {
        std::vector<Installed> out;
        const fs::path root = AppPaths::dataDir() / "agents";
        std::error_code ec;
        if (!fs::exists(root, ec)) {
            return out;
        }
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            std::ifstream marker(entry.path() / ".cmd");
            if (!marker) {
                continue;
            }
            std::string cmd;
            std::string name;
            std::getline(marker, cmd);
            std::getline(marker, name);
            const std::string id = entry.path().filename().string();
            out.push_back({id, name.empty() ? id : name});
        }
        return out;
    }

    std::vector<AcpRegistryAgent> fetch(std::string& error) {
        std::vector<AcpRegistryAgent> out;
        const std::string body = httpGet(std::string(REGISTRY_HOST) + REGISTRY_PATH, error);
        if (body.empty()) {
            return out;
        }

        const std::string platform = platformKey();
        try {
            const json root = json::parse(body);
            for (const auto& entry : root.value("agents", json::array())) {
                AcpRegistryAgent agent;
                agent.id = entry.value("id", "");
                agent.name = entry.value("name", agent.id);
                agent.description = entry.value("description", "");
                agent.version = entry.value("version", "");
                if (agent.id.empty()) {
                    continue;
                }

                const json dist = entry.value("distribution", json::object());
                agent.npmPackage = dist.value("npx", json::object()).value("package", "");
                agent.pyPackage = dist.value("uvx", json::object()).value("package", "");

                const json binaries = dist.value("binary", json::object());
                if (binaries.contains(platform)) {
                    const json& b = binaries[platform];
                    agent.hasBinary = true;
                    agent.archiveUrl = b.value("archive", "");
                    agent.archiveSha256 = b.value("sha256", "");
                    agent.binaryCmd = b.value("cmd", "");
                }
                out.push_back(std::move(agent));
            }
        } catch (const std::exception& e) {
            error = std::string("could not parse the registry: ") + e.what();
            return {};
        }
        return out;
    }

    bool installBinary(const AcpRegistryAgent& agent, std::string& error) {
        if (!agent.hasBinary || agent.archiveUrl.empty()) {
            error = agent.name + " has no prebuilt binary for " + platformKey();
            return false;
        }
        // keep the chmod'd/launched file inside the install dir
        if (agent.binaryCmd.empty() || agent.binaryCmd.front() == '/' ||
            agent.binaryCmd.find("..") != std::string::npos) {
            error = "refusing suspicious binary path in registry: " + agent.binaryCmd;
            return false;
        }

        const std::string archive = httpGet(agent.archiveUrl, error);
        if (archive.empty()) {
            return false;
        }

        // never unpack an executable we did not verify
        if (!agent.archiveSha256.empty()) {
            const std::string actual = sha256Hex(archive);
            if (actual != agent.archiveSha256) {
                error = "checksum mismatch for " + agent.name + " (expected " +
                        agent.archiveSha256 + ", got " + actual + ")";
                return false;
            }
        }

        const fs::path dir = installDir(agent.id);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (ec) {
            error = "could not create " + dir.string() + ": " + ec.message();
            return false;
        }

        const bool isZip = agent.archiveUrl.ends_with(".zip");
        const fs::path archivePath = dir / (isZip ? "agent.zip" : "agent.tar.gz");
        {
            std::ofstream out(archivePath, std::ios::binary);
            if (!out) {
                error = "could not write " + archivePath.string();
                return false;
            }
            out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
        }

        // tar and unzip ship with macOS and linux; no archive library needed
        ProcessSpec spec;
        spec.args =
            isZip
                ? std::vector<std::string>{"unzip", "-o",        "-q", archivePath.string(),
                                           "-d",    dir.string()}
                : std::vector<std::string>{"tar", "-xzf", archivePath.string(), "-C", dir.string()};
        const ProcessResult unpack = ProcessRunner::run(spec);
        fs::remove(archivePath, ec);
        if (!unpack.success || unpack.exitCode != 0) {
            error = "could not unpack the archive: " +
                    (unpack.output.empty() ? unpack.errorMessage : unpack.output);
            return false;
        }

        const fs::path exe = dir / agent.binaryCmd;
        if (!fs::exists(exe, ec)) {
            error = "the archive did not contain " + agent.binaryCmd;
            return false;
        }
        fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec, fs::perm_options::add,
                        ec);

        // remember how to launch it, so a later run needs no registry lookup
        std::ofstream marker(dir / ".cmd");
        marker << agent.binaryCmd << "\n" << agent.name << "\n" << agent.version << "\n";

        spdlog::info("installed ACP agent {} {} to {}", agent.id, agent.version, dir.string());
        return true;
    }

} // namespace AcpRegistry

void AcpRegistryClient::startFetch() {
    if (fetchOp_.isRunning()) {
        return;
    }
    error_.clear();
    fetchOp_.start([] {
        FetchResult result;
        result.agents = AcpRegistry::fetch(result.error);
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
        if (AcpRegistry::installBinary(agent, result.error)) {
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
