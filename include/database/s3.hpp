#pragma once

#include "async_helper.hpp"
#include "db_interface.hpp"
#include "s3_client.hpp"
#include <functional>
#include <memory>
#include <mutex>

// an S3 (or compatible) endpoint as a sidebar connection: buckets are its children,
// each opens in an S3BrowserTab. host = endpoint, database = region, username/password =
// access key / secret, sslmode Disable = plain http
class S3Database final : public DatabaseInterface {
public:
    explicit S3Database(const DatabaseConnectionInfo& info) {
        connectionInfo = info;
    }
    ~S3Database() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // null when disconnected; safe to use from worker threads
    [[nodiscard]] std::shared_ptr<const S3Client> client() const {
        std::lock_guard lock(mutex_);
        return client_;
    }

    // bucket list, loaded off the UI thread
    void loadBuckets();
    void checkBucketsStatus();
    [[nodiscard]] bool isLoadingBuckets() const {
        return bucketsOp_.isRunning();
    }
    [[nodiscard]] bool areBucketsLoaded() const {
        return bucketsLoaded_;
    }
    [[nodiscard]] const std::vector<S3Bucket>& buckets() const {
        return buckets_;
    }
    [[nodiscard]] const std::string& bucketsError() const {
        return bucketsError_;
    }

    // create / delete a bucket off the UI thread; the list reloads after. the error, if
    // any, is handed out once by takeChangeError()
    void changeBuckets(std::function<void(const S3Client&)> work);
    [[nodiscard]] bool isChangingBuckets() const {
        return changeOp_.isRunning();
    }
    std::string takeChangeError() {
        return std::exchange(changeError_, {});
    }

    [[nodiscard]] bool hasPendingAsyncWork() const override {
        return isConnecting() || bucketsOp_.isRunning() || changeOp_.isRunning();
    }

    static S3Config configFor(const DatabaseConnectionInfo& info);

private:
    struct BucketsResult {
        std::vector<S3Bucket> buckets;
        std::string error;
    };

    std::shared_ptr<const S3Client> client_;
    mutable std::mutex mutex_;
    AsyncOperation<BucketsResult> bucketsOp_;
    AsyncOperation<std::string> changeOp_;
    std::string changeError_;
    std::vector<S3Bucket> buckets_;
    std::string bucketsError_;
    bool bucketsLoaded_ = false;
};
