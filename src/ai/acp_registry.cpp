#include "ai/acp_registry.hpp"
#include "utils/app_paths.hpp"
#include "utils/process_runner.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <fstream>
#include <functional>
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

        // incremental sha-256, so archives are hashed as they stream to disk
        class Sha256 {
        public:
            Sha256() : ctx_(EVP_MD_CTX_new()) {
                if (ctx_) {
                    EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr);
                }
            }
            ~Sha256() {
                EVP_MD_CTX_free(ctx_);
            }
            void update(const char* data, size_t n) {
                if (ctx_) {
                    EVP_DigestUpdate(ctx_, data, n);
                }
            }
            std::string hex() {
                unsigned char digest[EVP_MAX_MD_SIZE];
                unsigned int len = 0;
                std::string out;
                if (ctx_ && EVP_DigestFinal_ex(ctx_, digest, &len) == 1) {
                    static constexpr char HEX[] = "0123456789abcdef";
                    for (unsigned int i = 0; i < len; ++i) {
                        out += HEX[digest[i] >> 4];
                        out += HEX[digest[i] & 0x0F];
                    }
                }
                return out;
            }

        private:
            EVP_MD_CTX* ctx_;
        };

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

        // streams the body to `sink`; agent archives run to hundreds of MB
        bool httpDownload(const std::string& url,
                          const std::function<void(const char*, size_t)>& sink,
                          std::string& error) {
            const auto [host, path] = splitUrl(url);
            if (host.empty()) {
                error = "bad url: " + url;
                return false;
            }
            httplib::Client client(host);
            client.set_follow_location(true); // release assets redirect to a CDN
            client.set_connection_timeout(15);
            client.set_read_timeout(120);

            auto res = client.Get(path, [&](const char* data, size_t n) {
                sink(data, n);
                return true;
            });
            if (!res) {
                error = "download failed: " + url + " (" + httplib::to_string(res.error()) + ")";
                return false;
            }
            if (res->status != 200) {
                error = "http " + std::to_string(res->status) + " for " + url;
                return false;
            }
            return true;
        }

        std::string httpGet(const std::string& url, std::string& error) {
            std::string body;
            if (!httpDownload(
                    url, [&](const char* data, size_t n) { body.append(data, n); }, error)) {
                return "";
            }
            return body;
        }

        // download an archive into dir (wiped first), verifying sha256 when given
        bool fetchAndUnpack(const std::string& url, const std::string& sha256, const fs::path& dir,
                            std::string& error) {
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
                Sha256 hash;
                const bool ok = httpDownload(
                    url,
                    [&](const char* data, size_t n) {
                        out.write(data, static_cast<std::streamsize>(n));
                        hash.update(data, n);
                    },
                    error);
                out.close();
                if (!ok || !out) {
                    if (ok) {
                        error = "could not write " + archivePath.string();
                    }
                    fs::remove_all(dir, ec);
                    return false;
                }
                // verified whenever the registry publishes a hash. Cursor and Antigravity
                // do not, so for them the HTTPS registry entry is the trust anchor
                const std::string actual = hash.hex();
                if (sha256.empty()) {
                    spdlog::warn(
                        "ACP: no checksum published for {}, unpacking unverified (sha256 {})", url,
                        actual);
                } else if (actual != sha256) {
                    error = "checksum mismatch for " + url + " (expected " + sha256 + ", got " +
                            actual + ")";
                    fs::remove_all(dir, ec);
                    return false;
                }
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
                // tar lists every failed entry; the last lines carry the reason
                std::string tail = unpack.output.empty() ? unpack.errorMessage : unpack.output;
                if (tail.size() > 400) {
                    tail = "..." + tail.substr(tail.size() - 400);
                }
                error = "could not unpack the archive: " + tail;
                fs::remove_all(dir, ec);
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

    // .cmd marker: {"cmd": "./x", "args": [...], "name": "...", "version": "..."}
    std::optional<std::vector<std::string>> installedCommand(const std::string& agentId) {
        std::ifstream in(installDir(agentId) / ".cmd");
        if (!in) {
            return std::nullopt;
        }
        json marker;
        try {
            in >> marker;
        } catch (const std::exception&) {
            return std::nullopt; // older one-line marker: download again
        }
        const std::string cmd = getString(marker, "cmd");
        const fs::path exe = installDir(agentId) / cmd;
        std::error_code ec;
        if (cmd.empty() || !fs::exists(exe, ec)) {
            return std::nullopt;
        }
        std::vector<std::string> argv{exe.string()};
        for (const auto& arg : marker.value("args", json::array())) {
            if (arg.is_string()) {
                argv.push_back(arg.get<std::string>());
            }
        }
        return argv;
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
                    for (const auto& arg : b.value("args", json::array())) {
                        if (arg.is_string()) {
                            agent.binaryArgs.push_back(arg.get<std::string>());
                        }
                    }
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
        marker << json{{"cmd", agent.binaryCmd},
                       {"args", agent.binaryArgs},
                       {"name", agent.name},
                       {"version", agent.version}}
                      .dump()
               << "\n";

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
    installedName_.clear();
    installOp_.start([agent] {
        InstallResult result;
        if (AcpRegistry::installBinary(agent, result.error)) {
            result.name = agent.name;
        }
        return result;
    });
}

void AcpRegistryClient::startInstallBun() {
    if (installOp_.isRunning()) {
        return;
    }
    error_.clear();
    installedName_.clear();
    installOp_.start([] {
        InstallResult result;
        if (AcpRegistry::installBun(result.error)) {
            result.name = "Bun";
        }
        return result;
    });
}

bool AcpRegistryClient::poll() {
    bool finished = false;
    finished |= fetchOp_.check([this](FetchResult result) {
        agents_ = std::move(result.agents);
        error_ = std::move(result.error);
        fetched_ = error_.empty();
    });
    finished |= installOp_.check([this](InstallResult result) {
        installedName_ = std::move(result.name);
        error_ = std::move(result.error);
    });
    return finished;
}

const AcpRegistryAgent* AcpRegistryClient::find(const std::string& id) const {
    for (const auto& agent : agents_) {
        if (agent.id == id) {
            return &agent;
        }
    }
    return nullptr;
}

bool AcpRegistryClient::isBusy() const {
    return fetchOp_.isRunning() || installOp_.isRunning();
}
