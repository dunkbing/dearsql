#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "utils/logger.hpp"

namespace AsyncOperationControl {
    inline std::atomic<bool>& skipWaitOnDestroy() {
        static std::atomic<bool> skipWait{false};
        return skipWait;
    }

    inline std::atomic<std::uint64_t>& cancelledTaskCount() {
        static std::atomic<std::uint64_t> count{0};
        return count;
    }

    inline std::uint64_t getCancelledTaskCount() {
        return cancelledTaskCount().load(std::memory_order_relaxed);
    }

    inline void resetCancelledTaskCount() {
        cancelledTaskCount().store(0, std::memory_order_relaxed);
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
    using CancellableTask = std::function<ResultType(std::stop_token)>;

    AsyncOperation() = default;
    ~AsyncOperation() {
        running = false;

        if (activeOperation.has_value()) {
            activeOperation->worker.request_stop();
        }
        for (auto& zombie : zombieOperations) {
            zombie.worker.request_stop();
        }

        if (AsyncOperationControl::skipWaitOnDestroy().load()) {
            releaseOperation(activeOperation);
            for (auto& zombie : zombieOperations) {
                releaseOperation(zombie);
            }
            return;
        }
        if (activeOperation.has_value() || !zombieOperations.empty()) {
            Logger::debug("AsyncOperation: waiting for pending futures on destruction");
        }
        waitForOperation(activeOperation);
        for (auto& zombie : zombieOperations) {
            waitForOperation(zombie);
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
        return startCancellable([task = std::move(task)](std::stop_token) { return task(); });
    }

    /**
     * Start an async operation with a cooperative cancellation token.
     */
    bool startCancellable(CancellableTask task) {
        if (isRunning()) {
            return false;
        }
        running = true;
        stashOperation(std::move(activeOperation));
        reapZombies();

        std::promise<ResultType> resultPromise;
        auto resultFuture = resultPromise.get_future();
        std::jthread worker([task = std::move(task), promise = std::move(resultPromise)](
                                std::stop_token stopToken) mutable {
            try {
                promise.set_value(task(stopToken));
            } catch (const std::exception& e) {
                Logger::error(std::string("AsyncOperation: task threw exception: ") + e.what());
                try {
                    promise.set_exception(std::current_exception());
                } catch (const std::exception& e2) {
                    Logger::error(
                        std::string("AsyncOperation: failed to set exception on promise: ") +
                        e2.what());
                } catch (...) {
                    Logger::error(
                        "AsyncOperation: failed to set exception on promise (unknown error)");
                }
            } catch (...) {
                Logger::error("AsyncOperation: task threw unknown exception");
                try {
                    promise.set_exception(std::current_exception());
                } catch (...) {
                    Logger::error(
                        "AsyncOperation: failed to set exception on promise (unknown error)");
                }
            }
        });

        activeOperation = OperationState{std::move(worker), std::move(resultFuture)};
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

        if (activeOperation.has_value() && activeOperation->future.valid() &&
            activeOperation->future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            running = false;
            waitForOperation(*activeOperation);
            if (callback) {
                callback(activeOperation->future.get());
                activeOperation.reset();
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
        const bool wasRunning = running.exchange(false);
        if (activeOperation.has_value()) {
            activeOperation->worker.request_stop();
        }
        for (auto& zombie : zombieOperations) {
            zombie.worker.request_stop();
        }
        if (wasRunning) {
            AsyncOperationControl::cancelledTaskCount().fetch_add(1, std::memory_order_relaxed);
        }
        reapZombies();
    }

    /**
     * Wait for the operation to complete and return the result.
     */
    ResultType waitAndGet() {
        if (activeOperation.has_value() && activeOperation->future.valid()) {
            running = false;
            waitForOperation(*activeOperation);
            ResultType result = activeOperation->future.get();
            activeOperation.reset();
            reapZombies();
            return result;
        }
        return ResultType{};
    }

private:
    struct OperationState {
        std::jthread worker;
        std::future<ResultType> future;
    };

    static void waitForOperation(OperationState& operation) {
        if (operation.worker.joinable()) {
            operation.worker.join();
        }
    }

    static void waitForOperation(std::optional<OperationState>& operation) {
        if (!operation.has_value()) {
            return;
        }
        waitForOperation(*operation);
    }

    static void releaseOperation(OperationState& operation) {
        if (operation.worker.joinable()) {
            operation.worker.detach();
        }
    }

    static void releaseOperation(std::optional<OperationState>& operation) {
        if (!operation.has_value()) {
            return;
        }
        releaseOperation(*operation);
        operation.reset();
    }

    static bool isReady(std::future<ResultType>& target) {
        return target.valid() &&
               target.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    void stashOperation(std::optional<OperationState>&& target) {
        if (!target.has_value()) {
            return;
        }

        if (target->future.valid() && !isReady(target->future)) {
            Logger::debug("AsyncOperation: stashing running future");
            zombieOperations.emplace_back(std::move(*target));
        } else {
            waitForOperation(*target);
        }
    }

    void reapZombies() {
        if (zombieOperations.empty()) {
            return;
        }
        Logger::debug(std::string("AsyncOperation: reapZombies ") +
                      std::to_string(zombieOperations.size()) + " completed futures");

        size_t reaped = 0;
        auto it = zombieOperations.begin();
        while (it != zombieOperations.end()) {
            if (!it->future.valid() || isReady(it->future)) {
                waitForOperation(*it);
                it = zombieOperations.erase(it);
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
    std::optional<OperationState> activeOperation;
    std::vector<OperationState> zombieOperations;
};
