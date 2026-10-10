#if !defined(_WIN32)

#include "utils/process_runner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <future>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace {
    using Clock = std::chrono::steady_clock;

    double secondsSince(const Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
} // namespace

// a child must not inherit the host's other fds: holding the write end of some
// other pipe (an agent's stdin, another runner's output) keeps its reader from EOF
TEST(ProcessRunnerTest, ChildDoesNotInheritHostPipes) {
#if !defined(__APPLE__)
    GTEST_SKIP() << "linux relies on O_CLOEXEC at the pipe's creator";
#else
    int fds[2];
    ASSERT_EQ(pipe(fds), 0); // no close-on-exec, like a careless library
    std::stop_source source;
    auto run = std::async(std::launch::async, [token = source.get_token()] {
        return ProcessRunner::run({.args = {"sleep", "5"}}, token);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    close(fds[1]);
    pollfd pfd{fds[0], POLLIN, 0};
    char c = 0;
    const bool eof = poll(&pfd, 1, 1000) == 1 && read(fds[0], &c, 1) == 0;
    source.request_stop();
    run.get();
    close(fds[0]);
    EXPECT_TRUE(eof) << "the child kept the pipe's write end open";
#endif
}

TEST(ProcessRunnerTest, StopTokenAndTimeoutKillTheChild) {
    std::stop_source source;
    auto start = Clock::now();
    auto run = std::async(std::launch::async, [token = source.get_token()] {
        return ProcessRunner::run({.args = {"sleep", "30"}}, token);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    source.request_stop();
    ASSERT_EQ(run.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    const ProcessResult cancelled = run.get();
    EXPECT_LT(secondsSince(start), 1.5);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_FALSE(cancelled.success);

    start = Clock::now();
    const ProcessResult timedOut =
        ProcessRunner::run({.args = {"sleep", "30"}, .timeout = std::chrono::milliseconds(300)});
    EXPECT_LT(secondsSince(start), 1.5);
    EXPECT_TRUE(timedOut.cancelled);
    EXPECT_NE(timedOut.errorMessage.find("timed out"), std::string::npos);
}

#endif
