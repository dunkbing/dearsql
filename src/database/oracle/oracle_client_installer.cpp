#include "database/oracle/oracle_client_installer.hpp"
#include <spdlog/spdlog.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

namespace fs = std::filesystem;

// Oracle Instant Client Basic Lite — direct download, no login required
// versions vary per platform; check Oracle's download pages for updates
static constexpr const char* kDownloadHost = "https://download.oracle.com";

#if defined(__linux__) && defined(__x86_64__)
static constexpr const char* kDownloadPath = "/otn_software/linux/instantclient/2326100/"
                                             "instantclient-basiclite-linux.x64-23.26.1.0.0.zip";
static constexpr const char* kArchiveName = "instantclient-basiclite-linux.x64-23.26.1.0.0.zip";
static constexpr const char* kExpectedDirName = "instantclient_23_26";
#elif defined(__linux__) && defined(__aarch64__)
static constexpr const char* kDownloadPath = "/otn_software/linux/instantclient/2326100/"
                                             "instantclient-basiclite-linux.arm64-23.26.1.0.0.zip";
static constexpr const char* kArchiveName = "instantclient-basiclite-linux.arm64-23.26.1.0.0.zip";
static constexpr const char* kExpectedDirName = "instantclient_23_26";
#elif defined(__APPLE__) && defined(__aarch64__)
static constexpr const char* kDownloadPath =
    "/otn_software/mac/instantclient/233023/"
    "instantclient-basiclite-macos.arm64-23.3.0.23.09-2.dmg";
static constexpr const char* kArchiveName =
    "instantclient-basiclite-macos.arm64-23.3.0.23.09-2.dmg";
static constexpr const char* kExpectedDirName = "instantclient_23_3";
#elif defined(__APPLE__) && defined(__x86_64__)
static constexpr const char* kDownloadPath =
    "/otn_software/mac/instantclient/1916000/"
    "instantclient-basiclite-macos.x64-19.16.0.0.0dbru.dmg";
static constexpr const char* kArchiveName = "instantclient-basiclite-macos.x64-19.16.0.0.0dbru.dmg";
static constexpr const char* kExpectedDirName = "instantclient_19_16";
#elif defined(_WIN32)
static constexpr const char* kDownloadPath = "/otn_software/nt/instantclient/2326100/"
                                             "instantclient-basiclite-windows.x64-23.26.1.0.0.zip";
static constexpr const char* kArchiveName = "instantclient-basiclite-windows.x64-23.26.1.0.0.zip";
static constexpr const char* kExpectedDirName = "instantclient_23_26";
#else
static constexpr const char* kDownloadPath = "";
static constexpr const char* kArchiveName = "";
static constexpr const char* kExpectedDirName = "";
#endif

static fs::path getBaseDir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home) {
        return fs::path(home) / ".dearsql" / "oracle-client";
    }
    return fs::path(".") / "oracle-client";
}

// cached install dir — cleared by startInstall() so post-install picks up the new path
static std::string sCachedInstallDir;

std::string OracleClientInstaller::getInstallDir() {
    if (!sCachedInstallDir.empty()) {
        return sCachedInstallDir;
    }

    fs::path base = getBaseDir();
    fs::path expected = base / kExpectedDirName;
    if (fs::exists(expected)) {
        sCachedInstallDir = expected.string();
        return sCachedInstallDir;
    }

    // fallback: scan for any instantclient_* directory (handles version mismatches)
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(base, ec)) {
        if (entry.is_directory() &&
            entry.path().filename().string().starts_with("instantclient_")) {
            sCachedInstallDir = entry.path().string();
            return sCachedInstallDir;
        }
    }

    return expected.string();
}

bool OracleClientInstaller::isInstalled() {
    const auto dir = fs::path(getInstallDir());
    if (!fs::exists(dir)) {
        return false;
    }

    // check for the main shared library
#if defined(__linux__)
    return fs::exists(dir / "libclntsh.so");
#elif defined(__APPLE__)
    return fs::exists(dir / "libclntsh.dylib");
#elif defined(_WIN32)
    return fs::exists(dir / "oci.dll");
#else
    return false;
#endif
}

std::string OracleClientInstaller::getDownloadUrl() {
    return std::string(kDownloadHost) + kDownloadPath;
}

std::string OracleClientInstaller::getArchiveName() {
    return kArchiveName;
}

void OracleClientInstaller::startInstall() {
    if (installOp.isRunning()) {
        return;
    }

    sCachedInstallDir.clear();

    if (std::strlen(kDownloadPath) == 0) {
        status = Status::Failed;
        error = "Oracle Instant Client auto-install is not supported on this platform";
        return;
    }

    status = Status::Downloading;
    statusMessage = "Downloading Oracle Instant Client...";
    error.clear();

    installOp.startCancellable(
        [this](std::stop_token stopToken) { return downloadAndExtract(stopToken); });
}

void OracleClientInstaller::cancel() {
    installOp.cancel();
    status = Status::Idle;
    statusMessage.clear();
}

void OracleClientInstaller::checkStatus() {
    installOp.check([this](bool success) {
        if (success) {
            status = Status::Done;
            statusMessage = "Oracle Instant Client installed successfully";
            spdlog::info(statusMessage);
        } else {
            status = Status::Failed;
            if (error.empty()) {
                error = "Installation failed";
            }
            statusMessage = error;
            spdlog::error("Oracle Client install failed: {}", error);
        }
    });
}

bool OracleClientInstaller::downloadAndExtract(std::stop_token stopToken) {
    // use canonical path for fresh install (not the fallback-scanned one)
    fs::path parentDir = getBaseDir();
    fs::path installDir = parentDir / kExpectedDirName;
    std::error_code ec;
    fs::create_directories(parentDir, ec);
    if (ec) {
        error = "Failed to create directory " + parentDir.string() + ": " + ec.message();
        return false;
    }

    fs::path archivePath = parentDir / kArchiveName;

    // download
    spdlog::info("Downloading Oracle Instant Client from {}{}", kDownloadHost, kDownloadPath);

    httplib::Client cli(kDownloadHost);
    cli.set_connection_timeout(15);
    cli.set_read_timeout(300);
    cli.set_follow_location(true);

    std::ofstream outFile(archivePath, std::ios::binary);
    if (!outFile.is_open()) {
        error = "Failed to create download file: " + archivePath.string();
        return false;
    }

    bool writeFailed = false;
    auto res = cli.Get(kDownloadPath, [&](const char* data, size_t len) -> bool {
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

    outFile.close();

    if (stopToken.stop_requested()) {
        fs::remove(archivePath, ec);
        return false;
    }

    if (!res || res->status != 200) {
        error =
            "Download failed: " + (res ? std::to_string(res->status) : std::string("no response"));
        fs::remove(archivePath, ec);
        return false;
    }
    if (writeFailed) {
        error = "Download failed while writing to disk";
        fs::remove(archivePath, ec);
        return false;
    }

    spdlog::info("Download complete, extracting...");
    status = Status::Extracting;
    statusMessage = "Extracting Oracle Instant Client...";

    // extract
    int ret = -1;
    std::string archiveStr = archivePath.string();
    std::string destStr = parentDir.string();

#if defined(__APPLE__)
    // macOS uses .dmg — mount, copy, unmount
    if (archiveStr.ends_with(".dmg")) {
        std::string mountPoint = destStr + "/oracle_dmg_mount";
        fs::create_directories(mountPoint, ec);

        std::string mountCmd = std::format("hdiutil attach '{}' -mountpoint '{}' -nobrowse -quiet",
                                           archiveStr, mountPoint);
        ret = std::system(mountCmd.c_str());
        if (ret == 0) {
            // copy all contents from mounted DMG into the install directory
            std::string icDir = (installDir).string();
            fs::create_directories(icDir, ec);
            std::string copyCmd = std::format("cp -R '{}/.' '{}'", mountPoint, icDir);
            ret = std::system(copyCmd.c_str());

            // always try to unmount
            std::string unmountCmd = std::format("hdiutil detach '{}' -quiet", mountPoint);
            std::system(unmountCmd.c_str());
        }
        fs::remove_all(mountPoint, ec);
    } else {
        ret = std::system(std::format("unzip -o -q '{}' -d '{}'", archiveStr, destStr).c_str());
    }
#elif defined(_WIN32)
    ret = std::system(
        std::format("powershell -Command \"Expand-Archive -Force -Path '{}' -DestinationPath "
                    "'{}'\"",
                    archiveStr, destStr)
            .c_str());
#else
    // Linux — zip archive
    ret = std::system(std::format("unzip -o -q '{}' -d '{}'", archiveStr, destStr).c_str());
#endif

    // clean up archive
    fs::remove(archivePath, ec);

    if (ret != 0) {
        error = "Failed to extract Oracle Instant Client archive";
        return false;
    }

    // clear cache so isInstalled() picks up the freshly extracted dir
    sCachedInstallDir.clear();

    if (!isInstalled()) {
        error = "Extraction completed but Oracle Client libraries not found in expected location";
        return false;
    }

#if defined(__linux__)
    // Oracle Instant Client requires libaio.so.1 on Linux.
    // bundle it into the install dir so ODPI-C finds it via oracleClientLibDir
    statusMessage = "Installing libaio dependency...";
    ensureLibaio(installDir);
#endif

    spdlog::info("Oracle Instant Client installed to: {}", installDir.string());
    return true;
}

#if defined(__linux__)
void OracleClientInstaller::ensureLibaio(const fs::path& installDir) {
    fs::path target = installDir / "libaio.so.1";
    if (fs::exists(target)) {
        // verify the bundled file has the correct ELF SONAME ("libaio.so.1").
        // Ubuntu 24.04+ ships libaio.so.1t64 with SONAME "libaio.so.1t64" which
        // Oracle's libclntsh.so won't accept via DT_NEEDED.
        std::string checkCmd =
            "objdump -p '" + target.string() + "' 2>/dev/null | grep -q 'SONAME.*libaio\\.so\\.1$'";
        if (std::system(checkCmd.c_str()) == 0) {
            return; // correct SONAME
        }
        spdlog::warn("Bundled libaio.so.1 has wrong SONAME, replacing...");
        std::error_code ec;
        fs::remove(target, ec);
    }

    // helper: check if a file has ELF SONAME exactly "libaio.so.1"
    auto hasCorrectSoname = [](const fs::path& file) -> bool {
        std::string cmd =
            "objdump -p '" + file.string() + "' 2>/dev/null | grep -q 'SONAME.*libaio\\.so\\.1$'";
        return std::system(cmd.c_str()) == 0;
    };

    // try to find libaio.so.1 on the system with correct SONAME.
    // Ubuntu 24.04+ renamed libaio to libaio.so.1t64 with SONAME "libaio.so.1t64"
    // which Oracle's libclntsh.so won't accept (it needs SONAME "libaio.so.1").
    for (const auto& candidate : {
             "/usr/lib/x86_64-linux-gnu/libaio.so.1",
             "/usr/lib/aarch64-linux-gnu/libaio.so.1",
             "/usr/lib64/libaio.so.1",
             "/usr/lib/libaio.so.1",
             "/lib/x86_64-linux-gnu/libaio.so.1",
             "/lib64/libaio.so.1",
         }) {
        if (!fs::exists(candidate)) {
            continue;
        }
        // resolve symlinks and check SONAME of the actual file
        std::error_code ec;
        auto resolved = fs::canonical(candidate, ec);
        if (ec || !hasCorrectSoname(resolved)) {
            continue;
        }
        fs::copy_file(candidate, target, ec);
        if (!ec) {
            spdlog::info("Bundled libaio from {}", candidate);
            return;
        }
    }

    // last resort: download libaio from Ubuntu package archive and extract the .so
    spdlog::info("libaio not found on system, downloading from package archive...");
    if (downloadLibaio(installDir)) {
        return;
    }

    spdlog::warn("Could not obtain libaio.so.1. Oracle Client may fail to load.");
}

bool OracleClientInstaller::downloadLibaio(const fs::path& installDir) {
    fs::path target = installDir / "libaio.so.1";

    // determine architecture
#if defined(__x86_64__)
    const char* arch = "amd64";
    const char* libPath = "lib/x86_64-linux-gnu";
#elif defined(__aarch64__)
    const char* arch = "arm64";
    const char* libPath = "lib/aarch64-linux-gnu";
#else
    return false;
#endif

    // use Ubuntu 22.04 (jammy) package — has SONAME "libaio.so.1" which Oracle expects.
    // Ubuntu 24.04+ renamed to libaio.so.1t64 with different SONAME, causing mismatch.
    std::string debName = std::format("libaio1_0.3.112-13build1_{}.deb", arch);
    std::string debPath = std::format("/ubuntu/pool/main/liba/libaio/{}", debName);

    httplib::Client cli("https://archive.ubuntu.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    cli.set_follow_location(true);

    fs::path debFile = installDir / debName;
    std::ofstream out(debFile, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    bool writeFailed = false;
    auto res = cli.Get(debPath, [&](const char* data, size_t len) -> bool {
        out.write(data, static_cast<std::streamsize>(len));
        if (!out) {
            writeFailed = true;
            return false;
        }
        return true;
    });
    out.close();

    if (!res || res->status != 200 || writeFailed) {
        std::error_code ec;
        fs::remove(debFile, ec);
        spdlog::warn("Failed to download libaio package: {}",
                     res ? std::to_string(res->status) : "no response");
        return false;
    }

    // extract libaio shared library from the .deb
    // the deb contains usr/lib/<arch>/libaio.so.1.0.1 (versioned filename)
    std::string destDir = installDir.string();

    // extract all files matching libaio.so* from the data archive
    std::string extractCmd =
        "cd '" + destDir +
        "' && ("
        "ar p '" +
        debFile.string() +
        "' data.tar.zst 2>/dev/null | tar --zstd -xf - --wildcards '*/libaio.so*' 2>/dev/null || "
        "ar p '" +
        debFile.string() +
        "' data.tar.xz 2>/dev/null | tar -xJf - --wildcards '*/libaio.so*' 2>/dev/null || "
        "ar p '" +
        debFile.string() +
        "' data.tar.gz 2>/dev/null | tar -xzf - --wildcards '*/libaio.so*' 2>/dev/null"
        ")";

    int ret = std::system(extractCmd.c_str());

    std::error_code ec;
    fs::remove(debFile, ec);

    if (ret != 0) {
        spdlog::warn("Failed to extract libaio from package");
        return false;
    }

    // find the extracted .so file (e.g. usr/lib/x86_64-linux-gnu/libaio.so.1.0.1)
    // and copy it as libaio.so.1
    fs::path searchDir = installDir / "usr" / libPath;
    if (fs::exists(searchDir)) {
        for (const auto& entry : fs::directory_iterator(searchDir, ec)) {
            if (entry.path().filename().string().starts_with("libaio.so.1")) {
                fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
                break;
            }
        }
    }

    // clean up extracted directory tree
    fs::remove_all(installDir / "usr", ec);

    if (fs::exists(target)) {
        spdlog::info("Downloaded and bundled libaio (SONAME=libaio.so.1) from Ubuntu 22.04");
        return true;
    }

    return false;
}
#endif
