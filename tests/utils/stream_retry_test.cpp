#define LLM_RETRY_IMPLEMENTATION
#include "ai/stream_retry.hpp"
#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {
struct FakeResponse {
    int status;
    std::string retryAfter;
    std::string get_header_value(const std::string&) const { return retryAfter; }
};
using FakeResult = std::shared_ptr<FakeResponse>;

llm::RetryConfig fastConfig() {
    llm::RetryConfig cfg;
    cfg.base_delay_ms = 1;
    return cfg;
}
} // namespace

TEST(StreamRetryTest, RetriesRateLimitThenStreams) {
    std::vector<int> statuses = {429, 529, 200};
    size_t calls = 0;
    std::string lineBuffer = "{\"error\":";
    auto res = postWithRetry(
        std::stop_token{}, lineBuffer,
        [&] { return std::make_shared<FakeResponse>(FakeResponse{statuses[calls++], ""}); },
        fastConfig());
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(calls, 3u);
    EXPECT_TRUE(lineBuffer.empty());
}

TEST(StreamRetryTest, DoesNotRetryClientErrors) {
    size_t calls = 0;
    std::string lineBuffer;
    auto res = postWithRetry(
        std::stop_token{}, lineBuffer,
        [&] { ++calls; return std::make_shared<FakeResponse>(FakeResponse{401, ""}); },
        fastConfig());
    EXPECT_EQ(res->status, 401);
    EXPECT_EQ(calls, 1u);
}

TEST(StreamRetryTest, DoesNotRetryDroppedConnection) {
    size_t calls = 0;
    std::string lineBuffer;
    auto res = postWithRetry(
        std::stop_token{}, lineBuffer, [&] { ++calls; return FakeResult{}; }, fastConfig());
    EXPECT_FALSE(res);
    EXPECT_EQ(calls, 1u);
}

TEST(StreamRetryTest, GivesUpAfterMaxAttempts) {
    size_t calls = 0;
    std::string lineBuffer;
    auto res = postWithRetry(
        std::stop_token{}, lineBuffer,
        [&] { ++calls; return std::make_shared<FakeResponse>(FakeResponse{503, ""}); },
        fastConfig());
    EXPECT_EQ(res->status, 503);
    EXPECT_EQ(calls, 3u);
}

TEST(StreamRetryTest, CancelStopsTheWait) {
    std::stop_source source;
    size_t calls = 0;
    std::string lineBuffer;
    auto res = postWithRetry(source.get_token(), lineBuffer, [&] {
        ++calls;
        source.request_stop();
        return std::make_shared<FakeResponse>(FakeResponse{429, "120"});
    });
    EXPECT_EQ(res->status, 429);
    EXPECT_EQ(calls, 1u);
}
