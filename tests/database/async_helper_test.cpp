#include "database/async_helper.hpp"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>

class AsyncOperationTest : public ::testing::Test {
protected:
    void SetUp() override {
        AsyncOperationControl::skipWaitOnDestroy().store(false);
        AsyncOperationControl::resetCancelledTaskCount();
    }

    void TearDown() override {
        AsyncOperationControl::skipWaitOnDestroy().store(false);
        AsyncOperationControl::resetCancelledTaskCount();
    }
};

TEST_F(AsyncOperationTest, StartRejectsConcurrentStart) {
    AsyncOperation<int> operation;
    std::promise<void> releasePromise;
    auto releaseFuture = releasePromise.get_future().share();

    ASSERT_TRUE(operation.start([releaseFuture]() mutable {
        releaseFuture.wait();
        return 7;
    }));
    EXPECT_TRUE(operation.isRunning());
    EXPECT_FALSE(operation.start([]() { return 9; }));

    releasePromise.set_value();
    EXPECT_EQ(operation.waitAndGet(), 7);
    EXPECT_FALSE(operation.isRunning());
}

TEST_F(AsyncOperationTest, CheckInvokesCallbackWhenOperationCompletes) {
    AsyncOperation<int> operation;
    ASSERT_TRUE(operation.start([]() { return 42; }));

    int callbackValue = -1;
    bool completed = false;
    for (int i = 0; i < 100 && !completed; ++i) {
        completed = operation.check([&callbackValue](int value) { callbackValue = value; });
        if (!completed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    EXPECT_TRUE(completed);
    EXPECT_EQ(callbackValue, 42);
    EXPECT_FALSE(operation.isRunning());
}

TEST_F(AsyncOperationTest, CancelSetsStopTokenForCancellableTask) {
    AsyncOperation<bool> operation;
    std::promise<void> startedPromise;
    auto startedFuture = startedPromise.get_future();

    ASSERT_TRUE(operation.startCancellable([&startedPromise](std::stop_token stopToken) {
        startedPromise.set_value();
        while (!stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }));

    ASSERT_EQ(startedFuture.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    operation.cancel();
    EXPECT_FALSE(operation.isRunning());
    EXPECT_TRUE(operation.waitAndGet());
    EXPECT_EQ(AsyncOperationControl::getCancelledTaskCount(), 1u);
}

TEST_F(AsyncOperationTest, StartAfterCancelCanLaunchNewOperation) {
    AsyncOperation<int> operation;
    std::promise<void> unblockFirstPromise;
    auto unblockFirstFuture = unblockFirstPromise.get_future().share();

    ASSERT_TRUE(operation.start([unblockFirstFuture]() mutable {
        unblockFirstFuture.wait();
        return 1;
    }));

    operation.cancel();
    ASSERT_TRUE(operation.start([]() { return 2; }));
    EXPECT_EQ(operation.waitAndGet(), 2);

    unblockFirstPromise.set_value();
}

TEST_F(AsyncOperationTest, WaitAndGetReturnsDefaultWhenNoFutureExists) {
    AsyncOperation<int> operation;
    EXPECT_EQ(operation.waitAndGet(), 0);
}

TEST_F(AsyncOperationTest, CancelDoesNotIncrementCounterWhenOperationNotRunning) {
    AsyncOperation<int> operation;
    operation.cancel();
    EXPECT_EQ(AsyncOperationControl::getCancelledTaskCount(), 0u);
}

TEST_F(AsyncOperationTest, SkipWaitOnDestroyReturnsQuicklyForPendingOperation) {
    AsyncOperationControl::skipWaitOnDestroy().store(true);

    std::promise<void> releasePromise;
    auto releaseFuture = releasePromise.get_future().share();
    const auto startTime = std::chrono::steady_clock::now();
    {
        AsyncOperation<int> operation;
        ASSERT_TRUE(operation.start([releaseFuture]() mutable {
            releaseFuture.wait();
            return 11;
        }));
    }
    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    EXPECT_LT(elapsed, std::chrono::milliseconds(100));

    releasePromise.set_value();
}
