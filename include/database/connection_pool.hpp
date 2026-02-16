#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>

template <typename ConnHandle> class ConnectionPool {
public:
    using ConnFactory = std::function<ConnHandle()>;
    using ConnCloser = std::function<void(ConnHandle)>;
    using ConnValidator = std::function<bool(ConnHandle)>;

    ConnectionPool(size_t poolSize, ConnFactory factory, ConnCloser closer,
                   ConnValidator validator = nullptr)
        : factory_(std::move(factory)), closer_(std::move(closer)),
          validator_(std::move(validator)) {
        for (size_t i = 0; i < poolSize; ++i) {
            ConnHandle conn = factory_();
            all_.push_back(conn);
            available_.push(conn);
        }
    }

    ~ConnectionPool() {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();

        for (auto conn : all_) {
            if (closer_) {
                closer_(conn);
            }
        }
        all_.clear();
        // Drain available queue
        while (!available_.empty()) {
            available_.pop();
        }
    }

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // RAII session handle -- returns connection to pool on destruction
    class Session {
    public:
        Session(ConnectionPool& pool, ConnHandle conn) : pool_(pool), conn_(conn) {}

        ~Session() {
            if (conn_) {
                pool_.release(conn_);
            }
        }

        Session(Session&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
            other.conn_ = ConnHandle{};
        }

        Session& operator=(Session&& other) noexcept {
            if (this != &other) {
                if (conn_) {
                    pool_.release(conn_);
                }
                conn_ = other.conn_;
                other.conn_ = ConnHandle{};
            }
            return *this;
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        ConnHandle get() const {
            return conn_;
        }

    private:
        ConnectionPool& pool_;
        ConnHandle conn_;
    };

    Session acquire() {
        std::unique_lock lock(mutex_);

        constexpr auto timeout = std::chrono::seconds(30);
        if (!cv_.wait_for(lock, timeout, [this] { return !available_.empty() || shutdown_; })) {
            throw std::runtime_error("ConnectionPool: acquire timeout (30s)");
        }

        if (shutdown_) {
            throw std::runtime_error("ConnectionPool: pool is shutting down");
        }

        ConnHandle conn = available_.front();
        available_.pop();

        // Validate connection, replace if dead
        if (validator_ && !validator_(conn)) {
            if (closer_) {
                closer_(conn);
            }
            // Remove from all_ and create a replacement
            auto it = std::find(all_.begin(), all_.end(), conn);
            if (it != all_.end()) {
                *it = factory_();
                conn = *it;
            } else {
                conn = factory_();
                all_.push_back(conn);
            }
        }

        return Session(*this, conn);
    }

private:
    void release(ConnHandle conn) {
        {
            std::lock_guard lock(mutex_);
            if (!shutdown_) {
                available_.push(conn);
            }
        }
        cv_.notify_one();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ConnHandle> available_;
    std::vector<ConnHandle> all_;
    ConnFactory factory_;
    ConnCloser closer_;
    ConnValidator validator_;
    bool shutdown_ = false;
};
