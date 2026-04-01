#if defined(__linux__)

#include "application.hpp"
#include "config.hpp"
#include "database/async_helper.hpp"
#include "platform/alert.hpp"
#include "platform/linux_platform.hpp"
#include "platform/updater.hpp"
#include <spdlog/spdlog.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtk/gtk.h>
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

// download progress tracking
static std::atomic<size_t> sDownloadCurrent{0};
static std::atomic<size_t> sDownloadTotal{0};
static GtkWidget* sProgressWindow = nullptr;
static GtkWidget* sProgressBar = nullptr;
static guint sProgressTimerId = 0;

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
        spdlog::error("Failed to open file for SHA-256: {}", path.string());
        return std::nullopt;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        spdlog::error("Failed to create OpenSSL digest context");
        return std::nullopt;
    }

    auto freeCtx = [&]() { EVP_MD_CTX_free(ctx); };
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        spdlog::error("Failed to initialize SHA-256 digest");
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
            spdlog::error("Failed to update SHA-256 digest");
            freeCtx();
            return std::nullopt;
        }
    }

    if (!in.eof()) {
        spdlog::error("Failed while reading file for SHA-256: {}", path.string());
        freeCtx();
        return std::nullopt;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
        spdlog::error("Failed to finalize SHA-256 digest");
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
        spdlog::error("Update check failed: {}", res ? std::to_string(res->status) : "no response");
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
            spdlog::error("Update info missing required AppImage fields");
            return false;
        }
        if (!isValidSha256Hex(downloadSha256)) {
            spdlog::error("Update info missing valid AppImage SHA-256");
            return false;
        }

        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        sLatestVersion = std::move(version);
        sDownloadUrl = std::move(downloadUrl);
        sDownloadSha256 = std::move(downloadSha256);
        sReleaseNotes = std::move(releaseNotes);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse update info: {}", e.what());
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
        spdlog::error("Invalid download URL");
        return false;
    }

    httplib::Client cli("https://" + host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(300); // 5 min for large downloads
    cli.set_follow_location(true);

    std::string tempPath = snapshot.appImagePath + ".update";
    std::ofstream outFile(tempPath, std::ios::binary);
    if (!outFile.is_open()) {
        spdlog::error("Failed to open temp file for download: {}", tempPath);
        return false;
    }

    sDownloadCurrent.store(0, std::memory_order_relaxed);
    sDownloadTotal.store(0, std::memory_order_relaxed);

    bool writeFailed = false;
    auto res = cli.Get(
        path,
        [&](const char* data, size_t len) -> bool {
            if (stopToken.stop_requested()) {
                return false;
            }
            outFile.write(data, static_cast<std::streamsize>(len));
            if (!outFile) {
                writeFailed = true;
                return false;
            }
            return true;
        },
        [&](size_t current, size_t total) -> bool {
            if (stopToken.stop_requested()) {
                return false;
            }
            sDownloadCurrent.store(current, std::memory_order_relaxed);
            sDownloadTotal.store(total, std::memory_order_relaxed);
            return true;
        });

    const bool streamOkBeforeClose = outFile.good();
    outFile.close();

    if (stopToken.stop_requested()) {
        std::filesystem::remove(tempPath);
        return false;
    }

    if (!res || res->status != 200) {
        spdlog::error("Download failed: {}", res ? std::to_string(res->status) : "no response");
        std::filesystem::remove(tempPath);
        return false;
    }
    if (writeFailed || !streamOkBeforeClose) {
        spdlog::error("Download failed while writing AppImage to disk");
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
        spdlog::error("Downloaded AppImage SHA-256 mismatch");
        std::filesystem::remove(tempPath);
        return false;
    }

    // Replace old AppImage
    std::error_code ec;
    std::filesystem::rename(tempPath, snapshot.appImagePath, ec);
    if (ec) {
        spdlog::error("Failed to replace AppImage: {}", ec.message());
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
        spdlog::error("Updated AppImage is not executable: {}", ec.message());
        return false;
    }

    spdlog::info("AppImage updated successfully");
    return true;
}

// --- Progress dialog ---

static void closeProgressDialog() {
    if (sProgressTimerId) {
        g_source_remove(sProgressTimerId);
        sProgressTimerId = 0;
    }
    if (sProgressWindow) {
        gtk_window_destroy(GTK_WINDOW(sProgressWindow));
        sProgressWindow = nullptr;
        sProgressBar = nullptr;
    }
}

static gboolean updateProgressBar(gpointer) {
    if (!sProgressBar)
        return G_SOURCE_REMOVE;

    size_t current = sDownloadCurrent.load(std::memory_order_relaxed);
    size_t total = sDownloadTotal.load(std::memory_order_relaxed);

    if (total > 0) {
        double fraction = static_cast<double>(current) / static_cast<double>(total);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sProgressBar), fraction);

        double mb_current = static_cast<double>(current) / (1024.0 * 1024.0);
        double mb_total = static_cast<double>(total) / (1024.0 * 1024.0);
        auto text = std::format("{:.1f} / {:.1f} MB", mb_current, mb_total);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sProgressBar), text.c_str());
    } else {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(sProgressBar));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sProgressBar), "Downloading...");
    }

    return G_SOURCE_CONTINUE;
}

static void showProgressDialog() {
    auto* platform = dynamic_cast<LinuxPlatform*>(Application::getInstance().getPlatform());
    GtkWindow* parent = platform ? GTK_WINDOW(platform->getGtkWindow()) : nullptr;

    sProgressWindow = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(sProgressWindow), "Downloading Update");
    gtk_window_set_default_size(GTK_WINDOW(sProgressWindow), 350, -1);
    gtk_window_set_resizable(GTK_WINDOW(sProgressWindow), FALSE);
    gtk_window_set_modal(GTK_WINDOW(sProgressWindow), TRUE);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(sProgressWindow), parent);
    }

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);

    GtkWidget* label = gtk_label_new("Downloading update, please wait...");
    gtk_box_append(GTK_BOX(box), label);

    sProgressBar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(sProgressBar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(sProgressBar), "Starting...");
    gtk_box_append(GTK_BOX(box), sProgressBar);

    gtk_window_set_child(GTK_WINDOW(sProgressWindow), box);

    // prevent closing the window while downloading
    g_signal_connect(sProgressWindow, "close-request",
                     G_CALLBACK(+[](GtkWindow*, gpointer) -> gboolean { return TRUE; }), nullptr);

    gtk_window_present(GTK_WINDOW(sProgressWindow));

    // poll progress every 100ms
    sProgressTimerId = g_timeout_add(100, updateProgressBar, nullptr);
}

// --- Public API ---

void initializeUpdater() {
    const char* appImageEnv = std::getenv("APPIMAGE");
    if (appImageEnv && !std::string(appImageEnv).empty()) {
        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        sAppImagePath = appImageEnv;
        spdlog::info("AppImage updater initialized: {}", appImageEnv);
    } else {
        spdlog::debug("Not running as AppImage, download/restart disabled");
    }

    sManualCheck = false;
    sCheckOp.startCancellable(checkForUpdate);
}

void checkForUpdates() {
    {
        std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
        if (sAppImagePath.empty()) {
            Alert::show("Update Not Available",
                        "Auto-update is only available when running as an AppImage.");
            return;
        }
    }

    if (sDownloadOp.isRunning()) {
        Alert::show("Update In Progress",
                    "Update download already in progress. Please wait for it to finish.");
        return;
    }

    if (sCheckOp.isRunning()) {
        return;
    }

    sManualCheck = true;
    sCheckOp.startCancellable(checkForUpdate);
}

void pollUpdater() {
    sCheckOp.check([](bool success) {
        if (!success) {
            if (sManualCheck) {
                Alert::show("Update Check Failed",
                            "Could not check for updates. Please check your connection.");
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
            if (sManualCheck) {
                std::string detail =
                    "Current: " + std::string(APP_VERSION) + "\nLatest:  " + latestVersion;
                if (!releaseNotes.empty()) {
                    detail += "\n\n" + releaseNotes;
                }
                Alert::show("A new version is available!", detail,
                            {{"Download Update",
                              [] {
                                  if (!sDownloadOp.isRunning()) {
                                      showProgressDialog();
                                      sDownloadOp.startCancellable(downloadUpdate);
                                  }
                              },
                              AlertButton::Style::Default},
                             {"Later", nullptr, AlertButton::Style::Cancel}});
            }
        } else if (sManualCheck) {
            Alert::show("Up to Date",
                        std::string("You're running the latest version (") + APP_VERSION + ").");
        }
    });

    sDownloadOp.check([](bool success) {
        closeProgressDialog();
        if (success) {
            Alert::show("Update downloaded successfully!", "Restart to apply the update.",
                        {{"Restart Now",
                          [] {
                              std::string appImagePath;
                              {
                                  std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
                                  appImagePath = sAppImagePath;
                              }
                              spdlog::info("Restarting AppImage: {}", appImagePath);
                              execl(appImagePath.c_str(), appImagePath.c_str(), nullptr);
                              spdlog::error("Failed to restart AppImage");
                              Alert::show("Restart Failed",
                                          "Failed to restart. Please relaunch manually.");
                          },
                          AlertButton::Style::Default},
                         {"Later", nullptr, AlertButton::Style::Cancel}});
        } else {
            Alert::show("Download Failed", "Download failed. Please try again later.");
        }
    });
}

bool isUpdateAvailable() {
    std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
    if (sLatestVersion.empty())
        return false;
    return isNewerVersion(sLatestVersion, APP_VERSION);
}

std::string getLatestVersion() {
    std::lock_guard<std::mutex> lock(sUpdaterStateMutex);
    return sLatestVersion;
}

void cleanupUpdater() {}

#endif
