#pragma once

#include "utils/reaper.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

// every live pool registers here so a query blocked on a worker thread can be
// cancelled server-side without the caller knowing which backend it is
class ConnectionPoolBase {
public:
    using Cancel = std::function<void()>;

    virtual ~ConnectionPoolBase() {
        unregisterPool();
    }

    // cancels for whatever `worker` is currently running, across all pools. only
    // collected under the locks (handle copies keep the connections alive); the
    // caller runs them, since a cancel is network I/O
    static std::vector<Cancel> cancelsFor(std::thread::id worker) {
        std::vector<Cancel> out;
        if (worker == std::thread::id{})
            return out;
        std::lock_guard lock(registryMutex());
        for (auto* pool : registry())
            pool->collectInUseBy(worker, out);
        return out;
    }

    // cancel what `worker` runs, on the reaper: safe to call from the UI thread
    static void cancelQueriesOn(std::thread::id worker) {
        for (auto& cancel : cancelsFor(worker))
            Reaper::post(std::move(cancel));
    }

protected:
    void registerPool() {
        std::lock_guard lock(registryMutex());
        registry().push_back(this);
    }
    // derived destructors call this first, before their members go away
    void unregisterPool() {
        std::lock_guard lock(registryMutex());
        std::erase(registry(), this);
    }
    virtual void collectInUseBy(std::thread::id worker, std::vector<Cancel>& out) = 0;

private:
    static std::mutex& registryMutex() {
        static std::mutex m;
        return m;
    }
    static std::vector<ConnectionPoolBase*>& registry() {
        static std::vector<ConnectionPoolBase*> pools;
        return pools;
    }
};

template <typename ConnHandle>
class ConnectionPool : public ConnectionPoolBase,
                       public std::enable_shared_from_this<ConnectionPool<ConnHandle>> {
public:
    using ConnFactory = std::function<ConnHandle()>;
    using ConnCloser = std::function<void(ConnHandle)>;
    using ConnValidator = std::function<bool(ConnHandle)>;
    // best-effort cancel of the query running on a busy connection, called from
    // another thread (PQcancel, KILL QUERY, ...). optional
    using ConnCanceller = std::function<void(ConnHandle)>;

    static constexpr size_t DEFAULT_POOL_SIZE = 2;

    ConnectionPool(ConnFactory factory, ConnCloser closer, ConnValidator validator = nullptr,
                   ConnCanceller canceller = nullptr, size_t maxSize = DEFAULT_POOL_SIZE,
                   int maxReconnectAttempts = 3)
        : factory_(std::move(factory)), closer_(std::move(closer)),
          validator_(std::move(validator)), canceller_(std::move(canceller)),
          maxSize_(std::max<size_t>(1, maxSize)),
          maxReconnectAttempts_(std::max(1, maxReconnectAttempts)) {
        // eagerly create one connection so errors surface immediately
        ConnHandle conn = factory_();
        all_.push_back(conn);
        available_.push(conn);
        registerPool();
    }

    // runs where the last owner lets go (the reaper, or a worker that held a
    // copy): sessions are all back by then unless someone kept one without one
    ~ConnectionPool() override {
        unregisterPool();
        if (auto cancel = shutdown())
            cancel();
        drain();

        for (auto conn : all_) {
            if (closer_)
                closer_(conn);
        }
        all_.clear();
        while (!available_.empty())
            available_.pop();
    }

    // refuse new sessions and return a cancel for every busy connection (null
    // when none): the caller decides where that network call runs
    Cancel shutdown() {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();
        if (!canceller_ || owners_.empty())
            return nullptr;
        std::vector<ConnHandle> busy;
        for (const auto& conn : owners_ | std::views::keys)
            busy.push_back(conn);
        return [canceller = canceller_, busy = std::move(busy)] {
            for (const auto& conn : busy)
                canceller(conn);
        };
    }

    // wait until every session is back and no thread is inside acquire(). a cancel
    // that lands while a handle is still connecting (or between statements) is
    // lost, so the busy ones are cancelled again every 250ms
    void drain() {
        std::unique_lock lock(mutex_);
        const auto idle = [this] { return inUse_ == 0 && busy_ == 0; };
        while (!cv_.wait_for(lock, std::chrono::milliseconds(250), idle)) {
            if (!canceller_)
                continue;
            std::vector<ConnHandle> busy;
            for (const auto& conn : owners_ | std::views::keys)
                busy.push_back(conn);
            lock.unlock();
            for (const auto& conn : busy)
                canceller_(conn);
            lock.lock();
        }
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
            if (shutdown_)
                throw std::runtime_error("ConnectionPool: pool is shutting down");
            ++busy_; // the destructor waits for us from here on
            struct BusyGuard {
                ConnectionPool* p;
                ~BusyGuard() {
                    --p->busy_;
                    p->cv_.notify_all();
                }
            } guard{this}; // runs with mutex_ held: every exit below holds the lock

            if (available_.empty() && all_.size() < maxSize_) {
                // grow the pool on demand
                lock.unlock();
                ConnHandle newConn;
                try {
                    newConn = factory_();
                } catch (...) {
                    lock.lock();
                    throw;
                }
                lock.lock();
                if (!shutdown_) {
                    all_.push_back(newConn);
                    available_.push(newConn);
                } else if (closer_) {
                    closer_(newConn);
                }
            }

            constexpr auto timeout = std::chrono::seconds(30);
            if (!cv_.wait_for(lock, timeout, [this] { return !available_.empty() || shutdown_; }))
                throw std::runtime_error("ConnectionPool: acquire timeout (30s)");
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
                    cv_.notify_all();
                }
                throw;
            }
        }

        {
            std::lock_guard lock(mutex_);
            // shut down while we validated: shutdown() could not see this one to cancel
            if (shutdown_) {
                --inUse_;
                cv_.notify_all();
                throw std::runtime_error("ConnectionPool: pool is shutting down");
            }
            owners_[conn] = {std::this_thread::get_id(), ++nextTicket_};
        }
        return Session(*this, conn);
    }

private:
    // the cancel runs later, off the locks: it fires only if the connection is
    // still in the same checkout, so one handed to another worker meanwhile is
    // left alone (ponytail: a release in the instant before the call still slips)
    void collectInUseBy(std::thread::id worker, std::vector<Cancel>& out) override {
        if (!canceller_)
            return;
        std::lock_guard lock(mutex_);
        std::weak_ptr<ConnectionPool> self = this->weak_from_this();
        for (const auto& [conn, owner] : owners_) {
            if (owner.thread != worker)
                continue;
            out.push_back([self, canceller = canceller_, conn, ticket = owner.ticket] {
                if (auto pool = self.lock(); pool && !pool->stillCheckedOut(conn, ticket))
                    return;
                canceller(conn);
            });
        }
    }

    bool stillCheckedOut(const ConnHandle& conn, uint64_t ticket) {
        std::lock_guard lock(mutex_);
        auto it = owners_.find(conn);
        return it != owners_.end() && it->second.ticket == ticket;
    }

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
        std::lock_guard lock(mutex_);
        if (inUse_ > 0)
            --inUse_;
        owners_.erase(conn);
        if (!shutdown_)
            available_.push(conn);
        // notify under the lock: the destructor may free cv_ as soon as it wakes
        cv_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ConnHandle> available_;
    std::vector<ConnHandle> all_;
    struct Owner {
        std::thread::id thread;
        uint64_t ticket = 0; // one per checkout
    };
    std::unordered_map<ConnHandle, Owner> owners_; // in-use conn -> holder
    uint64_t nextTicket_ = 0;
    ConnFactory factory_;
    ConnCloser closer_;
    ConnValidator validator_;
    ConnCanceller canceller_;
    size_t maxSize_;
    int maxReconnectAttempts_;
    size_t inUse_ = 0;
    size_t busy_ = 0; // threads inside acquire() before they hold a connection
    bool shutdown_ = false;
};
