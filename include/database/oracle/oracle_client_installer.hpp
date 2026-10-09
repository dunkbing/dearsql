#pragma once

#include "database/async_helper.hpp"
#include <atomic>
#include <cstdint>
#include <string>

// downloads Oracle Instant Client Basic Lite into the app data dir off the UI
// thread; the work is libdearsql's dearsql::oracle::install()
class OracleClientInstaller {
public:
    enum class Status { Idle, Downloading, Extracting, Done, Failed };

    // point the library at the app's install dir and keep installs out of
    // connect; idempotent, called before anything touches the client
    static void configure();

    // where the client libs are/will be installed
    static std::string getInstallDir();
    static bool isInstalled();

    // true when the client cannot be loaded (missing or broken)
    static bool needsClientInstall();
    // retry loading the client on the next connect (after an install)
    static void resetContext();

    void startInstall();
    void cancel();
    // poll progress from the UI thread
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

private:
    AsyncOperation<std::string> installOp; // error text, empty on success
    Status status = Status::Idle;
    std::string statusMessage;
    std::string error;
    // written by the install thread
    std::atomic<Status> phase_{Status::Idle};
    std::atomic<int64_t> bytesDone_{0};
    std::atomic<int64_t> bytesTotal_{0};
};
