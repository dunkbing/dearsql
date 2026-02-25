#if defined(__linux__)

#include "platform/linux_updater.hpp"
#include "config.hpp"
#include "database/async_helper.hpp"
#include "ui/update_dialog.hpp"
#include "utils/logger.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;

// --- Static state ---
static std::mutex sUpdaterStateMutex;
static std::string sAppImagePath;
static std::string sLatestVersion;
static std::string sDownloadUrl;
static std::string sDownloadSha256;
static std::string sReleaseNotes;
static bool sManualCheck = false;
static AsyncOperation<bool> sCheckOp;
static AsyncOperation<bool> sDownloadOp;

struct DownloadSnapshot {
    std::string appImagePath;
    std::string downloadUrl;
    std::string sha256;
};

static std::string normalizeSha256(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) { return std::isspace(c) != 0; }),
                value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool isValidSha256Hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

static std::optional<std::string> computeFileSha256Hex(const std::filesystem::path& path,
                                                       std::stop_token stopToken) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        Logger::error("Failed to open file for SHA-256: " + path.string());
        return std::nullopt;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        Logger::error("Failed to create OpenSSL digest context");
        return std::nullopt;
    }

    auto freeCtx = [&]() { EVP_MD_CTX_free(ctx); };
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        Logger::error("Failed to initialize SHA-256 digest");
        freeCtx();
        return std::nullopt;
    }

    std::array<char, 64 * 1024> buffer{};
    while (in) {
        if (stopToken.stop_requested()) {
            freeCtx();
            return std::nullopt;
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = in.gcount();
        if (bytesRead > 0 &&
            EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(bytesRead)) != 1) {
            Logger::error("Failed to update SHA-256 digest");
            freeCtx();
            return std::nullopt;
        }
    }

    if (!in.eof()) {
        Logger::error("Failed while reading file for SHA-256: " + path.string());
        freeCtx();
        return std::nullopt;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
        Logger::error("Failed to finalize SHA-256 digest");
        freeCtx();
        return std::nullopt;
    }
    freeCtx();

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digestLen * 2);
    for (unsigned int i = 0; i < digestLen; ++i) {
        hex.push_back(kHexDigits[digest[i] >> 4]);
        hex.push_back(kHexDigits[digest[i] & 0x0F]);
    }
    return hex;
}

// --- Version comparison ---
// Returns true if `a` is newer than `b` (e.g., "0.2.0" > "0.1.9")
static bool isNewerVersion(const std::string& a, const std::string& b) {
    auto parseSegments = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::string segment;
        for (char c : v) {
            if (c == '.') {
                parts.push_back(segment.empty() ? 0 : std::stoi(segment));
                segment.clear();
            } else {
                segment += c;
            }
        }
        if (!segment.empty())
            parts.push_back(std::stoi(segment));
        return parts;
    };

    auto pa = parseSegments(a);
    auto pb = parseSegments(b);
    size_t len = std::max(pa.size(), pb.size());
    pa.resize(len, 0);
    pb.resize(len, 0);

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] > pb[i])
            return true;
        if (pa[i] < pb[i])
            return false;
    }
    return false;
}

// --- Background version check ---
static bool checkForUpdate(std::stop_token stopToken) {
    httplib::Client cli("https://dearsql.dev");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(15);

    auto res = cli.Get("/api/version.json");
    if (stopToken.stop_requested())
        return false;

    if (!res || res->status != 200) {
        Logger::error("Update check failed: " +
                      (res ? std::to_string(res->status) : "no response"));
        return false;
    }

    try {
        auto data = json::parse(res->body);
        std::string version = data.value("version", "");
        std::string releaseNotes = data.value("release_notes", "");

        // Find AppImage download URL
        std::string downloadUrl;
        std::string downloadSha256;
        if (data.contains("downloads") && data["downloads"].contains("appimage-x86_64")) {
            downloadUrl = data["downloads"]["appimage-x86_64"].value("url", "");
            downloadSha256 = data["downloads"]["appimage-x86_64"].value("sha256", "");
        }

        downloadSha256 = normalizeSha256(downloadSha256);

        if (version.empty() || downloadUrl.empty()) {
            Logger::error("Update info missing required AppImage fields");
            return false;
        }
        if (!isValidSha256Hex(downloadSha256)) {
            Logger::error("Update info missing valid AppImage SHA-256");
            return false;
        }

        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        sLatestVersion = std::move(version);
        sDownloadUrl = std::move(downloadUrl);
        sDownloadSha256 = std::move(downloadSha256);
        sReleaseNotes = std::move(releaseNotes);
        return true;
    } catch (const std::exception& e) {
        Logger::error(std::string("Failed to parse update info: ") + e.what());
        return false;
    }
}

// --- Background download ---
static bool downloadUpdate(std::stop_token stopToken) {
    DownloadSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        snapshot.appImagePath = sAppImagePath;
        snapshot.downloadUrl = sDownloadUrl;
        snapshot.sha256 = sDownloadSha256;
    }

    if (snapshot.downloadUrl.empty() || snapshot.appImagePath.empty())
        return false;

    // Parse URL into host + path
    // e.g., "https://pub-xxx.r2.dev/DearSQL-0.2.0-x86_64.AppImage"
    std::string url = snapshot.downloadUrl;
    std::string host;
    std::string path;

    // Strip scheme
    if (url.starts_with("https://")) {
        url = url.substr(8);
    } else if (url.starts_with("http://")) {
        url = url.substr(7);
    }

    auto slashPos = url.find('/');
    if (slashPos != std::string::npos) {
        host = url.substr(0, slashPos);
        path = url.substr(slashPos);
    } else {
        Logger::error("Invalid download URL");
        return false;
    }

    httplib::Client cli("https://" + host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(300); // 5 min for large downloads
    cli.set_follow_location(true);

    std::string tempPath = snapshot.appImagePath + ".update";
    std::ofstream outFile(tempPath, std::ios::binary);
    if (!outFile.is_open()) {
        Logger::error("Failed to open temp file for download: " + tempPath);
        return false;
    }

    bool writeFailed = false;
    auto res = cli.Get(path, [&](const char* data, size_t len) -> bool {
        if (stopToken.stop_requested()) {
            return false;
        }
        outFile.write(data, static_cast<std::streamsize>(len));
        if (!outFile) {
            writeFailed = true;
            return false;
        }
        return true;
    });

    const bool streamOkBeforeClose = outFile.good();
    outFile.close();

    if (stopToken.stop_requested()) {
        std::filesystem::remove(tempPath);
        return false;
    }

    if (!res || res->status != 200) {
        Logger::error("Download failed: " + (res ? std::to_string(res->status) : "no response"));
        std::filesystem::remove(tempPath);
        return false;
    }
    if (writeFailed || !streamOkBeforeClose) {
        Logger::error("Download failed while writing AppImage to disk");
        std::filesystem::remove(tempPath);
        return false;
    }

    auto actualSha256 = computeFileSha256Hex(tempPath, stopToken);
    if (!actualSha256.has_value()) {
        std::filesystem::remove(tempPath);
        return false;
    }
    if (stopToken.stop_requested()) {
        std::filesystem::remove(tempPath);
        return false;
    }
    if (*actualSha256 != snapshot.sha256) {
        Logger::error("Downloaded AppImage SHA-256 mismatch");
        std::filesystem::remove(tempPath);
        return false;
    }

    // Replace old AppImage
    std::error_code ec;
    std::filesystem::rename(tempPath, snapshot.appImagePath, ec);
    if (ec) {
        Logger::error("Failed to replace AppImage: " + ec.message());
        std::filesystem::remove(tempPath);
        return false;
    }

    // Ensure executable
    std::filesystem::permissions(snapshot.appImagePath,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    if (ec) {
        Logger::error("Updated AppImage is not executable: " + ec.message());
        return false;
    }

    Logger::info("AppImage updated successfully");
    return true;
}

// --- Public API ---

void initializeLinuxUpdater() {
    const char* appImageEnv = std::getenv("APPIMAGE");
    if (!appImageEnv || std::string(appImageEnv).empty()) {
        Logger::debug("Not running as AppImage, updater disabled");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        sAppImagePath = appImageEnv;
    }
    Logger::info(std::string("AppImage updater initialized: ") + appImageEnv);

    // Silent background check
    sManualCheck = false;
    sCheckOp.startCancellable(checkForUpdate);
}

void checkForUpdatesLinux() {
    {
        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        if (sAppImagePath.empty()) {
            UpdateDialog::instance().showError(
                "Auto-update is only available when running as an AppImage.");
            return;
        }
    }

    if (sDownloadOp.isRunning()) {
        UpdateDialog::instance().showError(
            "Update download already in progress. Please wait for it to finish.");
        return;
    }

    if (sCheckOp.isRunning()) {
        return;
    }

    sManualCheck = true;
    UpdateDialog::instance().showChecking();
    sCheckOp.startCancellable(checkForUpdate);
}

void pollLinuxUpdater() {
    // Poll version check
    sCheckOp.check([](bool success) {
        auto& dialog = UpdateDialog::instance();

        if (!success) {
            if (sManualCheck) {
                dialog.showError("Could not check for updates. Please check your connection.");
            }
            return;
        }

        std::string latestVersion;
        std::string releaseNotes;
        {
            std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
            latestVersion = sLatestVersion;
            releaseNotes = sReleaseNotes;
        }

        if (isNewerVersion(latestVersion, APP_VERSION)) {
            dialog.showUpdateAvailable(APP_VERSION, latestVersion, releaseNotes);
        } else if (sManualCheck) {
            dialog.showUpToDate();
        }
    });

    // Poll download
    sDownloadOp.check([](bool success) {
        auto& dialog = UpdateDialog::instance();
        if (success) {
            dialog.showDownloadComplete();
        } else {
            dialog.showError("Download failed. Please try again later.");
        }
    });

    // Handle download request
    auto& dialog = UpdateDialog::instance();
    if (dialog.wantsDownload()) {
        dialog.clearWantsDownload();
        if (!sDownloadOp.isRunning()) {
            dialog.showDownloading();
            sDownloadOp.startCancellable(downloadUpdate);
        }
    }

    // Handle restart request
    if (dialog.wantsRestart()) {
        dialog.clearWantsRestart();
        std::string appImagePath;
        {
            std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
            appImagePath = sAppImagePath;
        }
        Logger::info("Restarting AppImage: " + appImagePath);
        execl(appImagePath.c_str(), appImagePath.c_str(), nullptr);
        // If execl returns, it failed
        Logger::error("Failed to restart AppImage");
        dialog.showError("Failed to restart. Please relaunch manually.");
    }
}

#endif
