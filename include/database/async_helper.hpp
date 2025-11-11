#pragma once

#include <atomic>
#include <functional>
#include <future>

/**
 * Generic helper for managing async operations with futures.
 * Reduces boilerplate for async patterns used across database implementations.
 */
template <typename ResultType> class AsyncOperation {
public:
    using Task = std::function<ResultType()>;
    using Callback = std::function<void(ResultType)>;

    AsyncOperation() = default;

    // Non-copyable, non-movable (due to std::atomic)
    AsyncOperation(const AsyncOperation&) = delete;
    AsyncOperation& operator=(const AsyncOperation&) = delete;
    AsyncOperation(AsyncOperation&&) = delete;
    AsyncOperation& operator=(AsyncOperation&&) = delete;

    /**
     * Start an async operation. Returns false if already running.
     */
    bool start(Task task) {
        if (isRunning()) {
            return false;
        }
        running = true;
        future = std::async(std::launch::async, std::move(task));
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
    std::atomic<bool> running{false};
    std::future<ResultType> future;
};
