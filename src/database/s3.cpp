#include "database/s3.hpp"

S3Database::~S3Database() {
    connectionOp.wait();
    bucketsOp_.wait();
    changeOp_.wait();
}

S3Config S3Database::configFor(const DatabaseConnectionInfo& info) {
    S3Config c;
    c.endpoint = info.host;
    // a port that is not the scheme's default goes into the host
    const bool https = info.sslmode != SslMode::Disable;
    if (!c.endpoint.empty() && info.port > 0 && info.port != (https ? 443 : 80) &&
        c.endpoint.find(':') == std::string::npos)
        c.endpoint += ":" + std::to_string(info.port);
    c.https = https;
    c.region = info.database;
    c.accessKey = info.username;
    c.secretKey = info.password;
    return c;
}

std::pair<bool, std::string> S3Database::connect() {
    if (connected)
        return {true, ""};
    setAttemptedConnection(true);
    auto client = std::make_shared<const S3Client>(configFor(connectionInfo));
    try {
        // listing buckets checks the endpoint and the keys
        client->listBuckets();
    } catch (const std::exception& e) {
        spdlog::error("S3 connection failed: {}", e.what());
        setLastConnectionError(e.what());
        return {false, e.what()};
    }
    {
        std::lock_guard lock(mutex_);
        client_ = std::move(client);
    }
    connected = true;
    setLastConnectionError("");
    return {true, ""};
}

void S3Database::disconnect() {
    std::lock_guard lock(mutex_);
    client_.reset();
    connected = false;
}

void S3Database::refreshConnection() {
    disconnect();
    bucketsLoaded_ = false;
    buckets_.clear();
    startConnectionAsync();
}

void S3Database::loadBuckets() {
    if (bucketsOp_.isRunning())
        return;
    bucketsOp_.start([client = client()] {
        BucketsResult r;
        if (!client)
            return r;
        try {
            r.buckets = client->listBuckets();
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        return r;
    });
}

void S3Database::checkBucketsStatus() {
    changeOp_.check([this](std::string error) {
        changeError_ = std::move(error);
        loadBuckets();
    });
    bucketsOp_.check([this](BucketsResult r) {
        buckets_ = std::move(r.buckets);
        bucketsError_ = std::move(r.error);
        bucketsLoaded_ = true;
    });
}

void S3Database::changeBuckets(std::function<void(const S3Client&)> work) {
    if (changeOp_.isRunning())
        return;
    changeOp_.start([client = client(), work = std::move(work)]() -> std::string {
        if (!client)
            return "Not connected";
        try {
            work(*client);
        } catch (const std::exception& e) {
            return e.what();
        }
        return {};
    });
}
