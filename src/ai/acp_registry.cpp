#include "ai/acp_registry.hpp"
#include "utils/app_paths.hpp"
#include "utils/process_runner.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace AcpRegistry {

    namespace {
        constexpr const char* REGISTRY_HOST = "https://cdn.agentclientprotocol.com";
        constexpr const char* REGISTRY_PATH = "/registry/v1/latest/registry.json";

        std::string getString(const json& j, const char* key) {
            const auto it = j.find(key);
            return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

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

        // download an archive into dir (wiped first), verifying sha256 when given
        bool fetchAndUnpack(const std::string& url, const std::string& sha256, const fs::path& dir,
                            std::string& error) {
            const std::string archive = httpGet(url, error);
            if (archive.empty()) {
                return false;
            }

            // never unpack an executable we did not verify
            if (!sha256.empty()) {
                const std::string actual = sha256Hex(archive);
                if (actual != sha256) {
                    error = "checksum mismatch for " + url + " (expected " + sha256 + ", got " +
                            actual + ")";
                    return false;
                }
            }

            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            if (ec) {
                error = "could not create " + dir.string() + ": " + ec.message();
                return false;
            }

            const bool isZip = url.ends_with(".zip");
            const fs::path archivePath = dir / (isZip ? "archive.zip" : "archive.tar.gz");
            {
                std::ofstream out(archivePath, std::ios::binary);
                if (!out) {
                    error = "could not write " + archivePath.string();
                    return false;
                }
                out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
            }

            // Windows 10+ ships bsdtar; it handles both zip and compressed tar archives.
#if defined(_WIN32)
            const ProcessResult unpack = ProcessRunner::run(
                {.args = {"tar.exe", "-xf", archivePath.string(), "-C", dir.string()}});
#else
            const ProcessResult unpack =
                isZip ? ProcessRunner::run({.args = {"unzip", "-o", "-q", archivePath.string(),
                                                     "-d", dir.string()}})
                      : ProcessRunner::run(
                            {.args = {"tar", "-xzf", archivePath.string(), "-C", dir.string()}});
#endif
            fs::remove(archivePath, ec);
            if (!unpack.success) {
                error = "could not unpack the archive: " +
                        (unpack.output.empty() ? unpack.errorMessage : unpack.output);
                return false;
            }
            return true;
        }

        fs::path bunDir() {
            return installRoot() / "bun";
        }

        // release asset stem, e.g. "bun-darwin-aarch64"; bun says x64 where we say x86_64
        std::string bunAsset() {
            std::string key = platformKey();
            if (const auto pos = key.find("x86_64"); pos != std::string::npos) {
                key.replace(pos, 6, "x64");
            }
            return "bun-" + key;
        }

        fs::path bunExe() {
#if defined(_WIN32)
            return bunDir() / bunAsset() / "bun.exe";
#else
            return bunDir() / bunAsset() / "bun";
#endif
        }
    } // namespace

    std::string platformKey() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
        return "windows-aarch64";
#else
        return "windows-x86_64";
#endif
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

    fs::path installRoot() {
        return AppPaths::dataDir() / "agents";
    }

    fs::path installDir(const std::string& agentId) {
        return installRoot() / agentId;
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
        const fs::path root = installRoot();
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

    std::vector<Agent> fetch(std::string& error) {
        std::vector<Agent> out;
        const std::string body = httpGet(std::string(REGISTRY_HOST) + REGISTRY_PATH, error);
        if (body.empty()) {
            return out;
        }

        const std::string platform = platformKey();
        try {
            const json root = json::parse(body);
            for (const auto& entry : root.value("agents", json::array())) {
                Agent agent;
                agent.id = getString(entry, "id");
                agent.name = entry.value("name", agent.id);
                agent.description = getString(entry, "description");
                agent.version = getString(entry, "version");
                if (agent.id.empty()) {
                    continue;
                }

                const json dist = entry.value("distribution", json::object());
                agent.npmPackage = getString(dist.value("npx", json::object()), "package");
                agent.pyPackage = getString(dist.value("uvx", json::object()), "package");

                const json binaries = dist.value("binary", json::object());
                if (binaries.contains(platform)) {
                    const json& b = binaries[platform];
                    agent.hasBinary = true;
                    agent.archiveUrl = getString(b, "archive");
                    agent.archiveSha256 = getString(b, "sha256");
                    agent.binaryCmd = getString(b, "cmd");
                }
                out.push_back(std::move(agent));
            }
        } catch (const std::exception& e) {
            error = std::string("could not parse the registry: ") + e.what();
            return {};
        }
        return out;
    }

    bool installBinary(const Agent& agent, std::string& error) {
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

        const fs::path dir = installDir(agent.id);
        if (!fetchAndUnpack(agent.archiveUrl, agent.archiveSha256, dir, error)) {
            return false;
        }

        std::error_code ec;
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

        spdlog::info("ACP: installed agent {} {} to {}", agent.id, agent.version, dir.string());
        return true;
    }

    std::optional<std::string> bunPath() {
        std::ifstream marker(bunDir() / ".version");
        std::string version;
        std::getline(marker, version);
        std::error_code ec;
        if (version != BUN_VERSION || !fs::exists(bunExe(), ec)) {
            return std::nullopt; // missing or stale: installBun again
        }
        return bunExe().string();
    }

    std::vector<std::pair<std::string, std::string>> bunEnv() {
        if (!bunPath()) {
            return {};
        }
        return {{"BUN_INSTALL", bunDir().string()}};
    }

    bool installBun(std::string& error) {
        const std::string base =
            std::string("https://github.com/oven-sh/bun/releases/download/bun-v") + BUN_VERSION +
            "/";
        const std::string asset = bunAsset() + ".zip";

        // "<sha256>  <file>" per line; the checksum is mandatory here
        const std::string sums = httpGet(base + "SHASUMS256.txt", error);
        if (sums.empty()) {
            return false;
        }
        std::string sha;
        std::istringstream lines(sums);
        for (std::string line; std::getline(lines, line);) {
            if (line.ends_with(" " + asset)) {
                sha = line.substr(0, line.find(' '));
                break;
            }
        }
        if (sha.empty()) {
            error = "no checksum published for " + asset;
            return false;
        }

        // wipes the package cache too; fine for the rare version bump
        if (!fetchAndUnpack(base + asset, sha, bunDir(), error)) {
            return false;
        }
        std::error_code ec;
        if (!fs::exists(bunExe(), ec)) {
            error = "the archive did not contain " + bunExe().filename().string();
            return false;
        }
        fs::permissions(bunExe(), fs::perms::owner_exec | fs::perms::group_exec,
                        fs::perm_options::add, ec);
        std::ofstream marker(bunDir() / ".version");
        marker << BUN_VERSION << "\n";

        spdlog::info("ACP: installed bun {} to {}", BUN_VERSION, bunDir().string());
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

void AcpRegistryClient::startInstallBun() {
    if (installOp_.isRunning()) {
        return;
    }
    error_.clear();
    installedId_.clear();
    installOp_.start([] {
        InstallResult result;
        if (AcpRegistry::installBun(result.error)) {
            result.agentId = "Bun";
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
