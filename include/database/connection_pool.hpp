#pragma once

#include <algorithm>
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

    static constexpr size_t DEFAULT_POOL_SIZE = 2;

    ConnectionPool(ConnFactory factory, ConnCloser closer, ConnValidator validator = nullptr,
                   size_t maxSize = DEFAULT_POOL_SIZE, int maxReconnectAttempts = 3)
        : factory_(std::move(factory)), closer_(std::move(closer)),
          validator_(std::move(validator)), maxSize_(std::max<size_t>(1, maxSize)),
          maxReconnectAttempts_(std::max(1, maxReconnectAttempts)) {
        // eagerly create one connection so errors surface immediately
        ConnHandle conn = factory_();
        all_.push_back(conn);
        available_.push(conn);
    }

    ~ConnectionPool() {
        {
            std::unique_lock lock(mutex_);
            shutdown_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return inUse_ == 0; });
        }

        for (auto conn : all_) {
            if (closer_)
                closer_(conn);
        }
        all_.clear();
        while (!available_.empty())
            available_.pop();
    }

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    class Session {
    public:
        Session(ConnectionPool& pool, ConnHandle conn) : pool_(&pool), conn_(conn) {}

        ~Session() {
            if (conn_)
                pool_->release(conn_);
        }

        Session(Session&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
            other.conn_ = ConnHandle{};
        }

        Session& operator=(Session&& other) noexcept {
            if (this != &other) {
                if (conn_)
                    pool_->release(conn_);
                pool_ = other.pool_;
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
        ConnectionPool* pool_;
        ConnHandle conn_;
    };

    Session acquire() {
        ConnHandle conn;
        {
            std::unique_lock lock(mutex_);

            if (available_.empty() && !shutdown_ && all_.size() < maxSize_) {
                // grow the pool on demand
                lock.unlock();
                ConnHandle newConn = factory_();
                lock.lock();
                if (!shutdown_) {
                    all_.push_back(newConn);
                    available_.push(newConn);
                } else {
                    if (closer_)
                        closer_(newConn);
                }
            }

            constexpr auto timeout = std::chrono::seconds(30);
            if (!cv_.wait_for(lock, timeout, [this] { return !available_.empty() || shutdown_; })) {
                throw std::runtime_error("ConnectionPool: acquire timeout (30s)");
            }

            if (shutdown_)
                throw std::runtime_error("ConnectionPool: pool is shutting down");

            conn = available_.front();
            available_.pop();
            ++inUse_;
        }

        // validate + auto-reconnect outside the lock
        if (validator_ && !validator_(conn)) {
            try {
                conn = reconnect_(conn);
            } catch (...) {
                {
                    std::lock_guard lock(mutex_);
                    --inUse_;
                    available_.push(conn);
                }
                cv_.notify_all();
                throw;
            }
        }

        return Session(*this, conn);
    }

private:
    ConnHandle reconnect_(ConnHandle oldConn) {
        std::exception_ptr lastEx;
        for (int attempt = 0; attempt < maxReconnectAttempts_; ++attempt) {
            try {
                ConnHandle newConn = factory_();
                {
                    std::lock_guard lock(mutex_);
                    auto it = std::find(all_.begin(), all_.end(), oldConn);
                    if (it != all_.end())
                        *it = newConn;
                    else
                        all_.push_back(newConn);
                }
                if (closer_)
                    closer_(oldConn);
                return newConn;
            } catch (...) {
                lastEx = std::current_exception();
            }
        }
        std::rethrow_exception(lastEx);
    }

    void release(ConnHandle conn) {
        {
            std::lock_guard lock(mutex_);
            if (inUse_ > 0)
                --inUse_;
            if (!shutdown_)
                available_.push(conn);
        }
        cv_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ConnHandle> available_;
    std::vector<ConnHandle> all_;
    ConnFactory factory_;
    ConnCloser closer_;
    ConnValidator validator_;
    size_t maxSize_;
    int maxReconnectAttempts_;
    size_t inUse_ = 0;
    bool shutdown_ = false;
};
