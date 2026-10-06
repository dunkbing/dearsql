#include "database/oracle/oracle_client_installer.hpp"
#include "utils/app_paths.hpp"
#include <dearsql/oracle_installer.hpp>
#include <format>
#include <mutex>
#include <spdlog/spdlog.h>

void OracleClientInstaller::configure() {
    static std::once_flag once;
    std::call_once(once, [] {
        dearsql::oracle::setInstallRoot((AppPaths::dataDir() / "oracle-client").string());
        // the sidebar offers the install; a gui must not block or re-exec in connect
        dearsql::oracle::setClientOptions({.autoInstall = false, .reexecForLibraryPath = false});
    });
}

std::string OracleClientInstaller::getInstallDir() {
    configure();
    return dearsql::oracle::installDir();
}

bool OracleClientInstaller::isInstalled() {
    configure();
    return dearsql::oracle::isInstalled();
}

bool OracleClientInstaller::needsClientInstall() {
    configure();
    return dearsql::oracle::needsClientInstall();
}

void OracleClientInstaller::resetContext() {
    configure();
    dearsql::oracle::resetContext();
}

void OracleClientInstaller::startInstall() {
    if (installOp.isRunning())
        return;
    configure();
    if (dearsql::oracle::downloadUrl().empty()) {
        status = Status::Failed;
        error = "Oracle Instant Client auto-install is not supported on this platform";
        statusMessage = error;
        return;
    }
    status = phase_ = Status::Downloading;
    bytesDone_ = bytesTotal_ = 0;
    statusMessage = "Downloading Oracle Instant Client...";
    error.clear();

    installOp.startCancellable([this](std::stop_token stop) {
        auto [ok, err] = dearsql::oracle::install(
            [this](const dearsql::oracle::Progress& p) {
                if (p.phase == "downloading") {
                    bytesDone_ = p.bytesDownloaded;
                    bytesTotal_ = p.bytesTotal;
                } else if (p.phase == "extracting" || p.phase == "installing-libaio") {
                    phase_ = Status::Extracting;
                }
            },
            [&stop] { return stop.stop_requested(); });
        return ok ? std::string() : (err.empty() ? std::string("Installation failed") : err);
    });
}

void OracleClientInstaller::cancel() {
    installOp.cancel();
    status = Status::Idle;
    statusMessage.clear();
}

void OracleClientInstaller::checkStatus() {
    if (installOp.isRunning()) {
        status = phase_;
        if (status == Status::Extracting) {
            statusMessage = "Extracting Oracle Instant Client...";
        } else if (const int64_t total = bytesTotal_; total > 0) {
            statusMessage =
                std::format("Downloading Oracle Instant Client... {}%", bytesDone_ * 100 / total);
        }
    }
    installOp.check([this](const std::string& err) {
        if (err.empty()) {
            status = Status::Done;
            statusMessage = "Oracle Instant Client installed successfully";
            spdlog::info("Oracle Instant Client installed to {}", getInstallDir());
        } else {
            status = Status::Failed;
            error = err;
            statusMessage = error;
            spdlog::error("Oracle Client install failed: {}", error);
        }
    });
}
