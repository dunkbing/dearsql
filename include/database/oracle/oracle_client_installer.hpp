#pragma once

#include "database/async_helper.hpp"
#include <filesystem>
#include <functional>
#include <string>

// downloads and extracts Oracle Instant Client Basic Lite to the app data dir
class OracleClientInstaller {
public:
    enum class Status { Idle, Downloading, Extracting, Done, Failed };

    // returns the path where Oracle Client libs are/will be installed
    static std::string getInstallDir();

    // returns true if Oracle Client is already installed locally
    static bool isInstalled();

    // start async download + extract
    void startInstall();

    // cancel a running install
    void cancel();

    // poll progress — call from UI thread
    void checkStatus();

    [[nodiscard]] Status getStatus() const {
        return status;
    }
    [[nodiscard]] const std::string& getStatusMessage() const {
        return statusMessage;
    }
    [[nodiscard]] const std::string& getError() const {
        return error;
    }
    [[nodiscard]] bool isRunning() const {
        return installOp.isRunning();
    }

#if defined(__linux__)
    // ensure libaio.so.1 is present in the install dir (downloads if needed)
    static void ensureLibaio(const std::filesystem::path& installDir);
#endif

private:
    AsyncOperation<bool> installOp;
    Status status = Status::Idle;
    std::string statusMessage;
    std::string error;

    static std::string getDownloadUrl();
    static std::string getArchiveName();
    bool downloadAndExtract(std::stop_token stopToken);
#if defined(__linux__)
    static bool downloadLibaio(const std::filesystem::path& installDir);
#endif
};
