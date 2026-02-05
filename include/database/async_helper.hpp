#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "utils/logger.hpp"

namespace AsyncOperationControl {
    inline std::atomic<bool>& skipWaitOnDestroy() {
        static std::atomic<bool> skipWait{false};
        return skipWait;
    }
} // namespace AsyncOperationControl

/**
 * Generic helper for managing async operations with futures.
 * Reduces boilerplate for async patterns used across database implementations.
 */
template <typename ResultType> class AsyncOperation {
public:
    using Task = std::function<ResultType()>;
    using Callback = std::function<void(ResultType)>;
    using CancelFlag = std::shared_ptr<std::atomic<bool>>;
    using CancellableTask = std::function<ResultType(const CancelFlag&)>;

    AsyncOperation() = default;
    ~AsyncOperation() {
        if (AsyncOperationControl::skipWaitOnDestroy().load()) {
            leakFuture(std::move(future));
            for (auto& zombie : zombieFutures) {
                leakFuture(std::move(zombie));
            }
            return;
        }
        if (future.valid() || !zombieFutures.empty()) {
            Logger::debug("AsyncOperation: waiting for pending futures on destruction");
        }
        waitForFuture(future);
        for (auto& zombie : zombieFutures) {
            waitForFuture(zombie);
        }
    }

    // Non-copyable, non-movable (due to std::atomic)
    AsyncOperation(const AsyncOperation&) = delete;
    AsyncOperation& operator=(const AsyncOperation&) = delete;
    AsyncOperation(AsyncOperation&&) = delete;
    AsyncOperation& operator=(AsyncOperation&&) = delete;

    /**
     * Start an async operation. Returns false if already running.
     */
    bool start(Task task) {
        return startCancellable([task = std::move(task)](const CancelFlag&) { return task(); });
    }

    /**
     * Start an async operation with a shared cancellation flag.
     */
    bool startCancellable(CancellableTask task) {
        if (isRunning()) {
            return false;
        }
        running = true;
        cancelFlag = std::make_shared<std::atomic<bool>>(false);
        stashFuture(std::move(future));
        reapZombies();
        future = std::async(std::launch::async,
                            [task = std::move(task), flag = cancelFlag]() { return task(flag); });
        return true;
    }

    /**
     * Check if the operation is complete and invoke callback if provided.
     * Returns true if the operation completed during this check.
     */
    bool check(Callback callback = nullptr) {
        if (!running) {
            return false;
        }

        if (future.valid() &&
            future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            running = false;
            if (callback) {
                callback(future.get());
            }
            reapZombies();
            return true;
        }
        return false;
    }

    /**
     * Check if operation is currently running.
     */
    [[nodiscard]] bool isRunning() const {
        return running.load();
    }

    /**
     * Cancel the operation (doesn't actually stop the thread, just marks as not running).
     */
    void cancel() {
        running = false;
        if (cancelFlag) {
            cancelFlag->store(true);
        }
        reapZombies();
    }

    /**
     * Wait for the operation to complete and return the result.
     */
    ResultType waitAndGet() {
        if (future.valid()) {
            running = false;
            return future.get();
        }
        return ResultType{};
    }

private:
    static void waitForFuture(std::future<ResultType>& target) {
        if (target.valid()) {
            target.wait();
        }
    }

    static void leakFuture(std::future<ResultType>&& target) {
        if (!target.valid()) {
            return;
        }
        static auto* leakedFutures = new std::vector<std::future<ResultType>>();
        leakedFutures->emplace_back(std::move(target));
    }

    void stashFuture(std::future<ResultType>&& target) {
        if (!target.valid()) {
            return;
        }

        if (target.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            Logger::debug("AsyncOperation: stashing running future");
            zombieFutures.emplace_back(std::move(target));
        }
    }

    void reapZombies() {
        Logger::debug(std::string("AsyncOperation: reapZombies ") +
                      std::to_string(zombieFutures.size()) + " completed futures");
        if (zombieFutures.empty()) {
            return;
        }

        size_t reaped = 0;
        auto it = zombieFutures.begin();
        while (it != zombieFutures.end()) {
            if (!it->valid() ||
                it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                it = zombieFutures.erase(it);
                ++reaped;
            } else {
                ++it;
            }
        }
        if (reaped > 0) {
            Logger::debug(std::string("AsyncOperation: reaped ") + std::to_string(reaped) +
                          " completed futures");
        }
    }

    std::atomic<bool> running{false};
    std::future<ResultType> future;
    CancelFlag cancelFlag;
    std::vector<std::future<ResultType>> zombieFutures;
};
