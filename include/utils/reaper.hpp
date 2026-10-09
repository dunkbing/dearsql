#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

// one background thread that runs teardown the UI thread must not wait for:
// destroying objects whose destructors join workers or close connections, and
// best-effort server-side cancels. jobs run in order. it never counts as running
// work for the frame loop, so an idle reaper does not keep frames rendering
class Reaper {
public:
    // run fn on the reaper; inline when already on it (a job posting more work)
    static void post(std::function<void()> fn) {
        if (!fn)
            return;
        auto& s = state();
        if (s.inlineMode || std::this_thread::get_id() == s.threadId.load()) {
            fn();
            return;
        }
        std::lock_guard lock(s.mutex);
        if (!s.started) {
            s.started = true;
            std::thread worker(loop);
            s.threadId = worker.get_id();
            worker.detach();
        }
        s.jobs.push_back(std::move(fn));
        s.cv.notify_all();
    }

    // destroy obj on the reaper (a unique_ptr, shared_ptr or any movable value)
    template <typename T> static void dispose(T&& obj) {
        using V = std::remove_cvref_t<T>;
        auto held = std::make_shared<V>(std::forward<T>(obj));
        post([held = std::move(held)]() mutable { held.reset(); });
    }

    // wait until every queued job has run; false when timeout passes first
    static bool drain(std::chrono::milliseconds timeout) {
        auto& s = state();
        std::unique_lock lock(s.mutex);
        return s.cv.wait_for(lock, timeout, [&] { return s.jobs.empty() && !s.running; });
    }

    // at exit, once drained: later jobs (static destruction) run on the caller
    static void runInline() {
        state().inlineMode = true;
    }

    [[nodiscard]] static bool idle() {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        return s.jobs.empty() && !s.running;
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::function<void()>> jobs;
        std::atomic<std::thread::id> threadId{};
        std::atomic<bool> inlineMode{false};
        bool started = false;
        bool running = false;
    };

    // leaked: the detached thread may still wait on it during static destruction
    static State& state() {
        static auto* s = new State;
        return *s;
    }

    static void loop() {
        auto& s = state();
        std::unique_lock lock(s.mutex);
        for (;;) {
            s.cv.wait(lock, [&] { return !s.jobs.empty(); });
            auto job = std::move(s.jobs.front());
            s.jobs.pop_front();
            s.running = true;
            lock.unlock();
            try {
                job();
                job = nullptr; // captures die here, not under the lock
            } catch (...) {
                job = nullptr;
            }
            lock.lock();
            s.running = false;
            s.cv.notify_all();
        }
    }
};
