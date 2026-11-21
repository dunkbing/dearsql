#pragma once

#include <llm_retry.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <string>

// a 408/429/5xx/529 arrives before any token is streamed, so the request can be
// sent again; a dropped connection mid-stream is not retried (text was shown)
template <typename Post>
auto postWithRetry(std::stop_token stopToken, std::string& lineBuffer, Post&& post,
                   const llm::RetryConfig& cfg = {}) {
    for (int attempt = 1;; ++attempt) {
        auto res = post();
        if (!res || res->status == 200 || attempt >= cfg.max_attempts ||
            stopToken.stop_requested() ||
            !llm::detail::is_retryable_code(res->status, cfg.retry_on_http_codes)) {
            return res;
        }
        lineBuffer.clear();

        double delayMs = llm::detail::compute_delay_ms(attempt, cfg);
        try {
            delayMs = std::max(delayMs, std::stod(res->get_header_value("retry-after")) * 1000.0);
        } catch (...) {
        }
        std::mutex m;
        std::unique_lock lock(m);
        std::condition_variable_any().wait_for(
            lock, stopToken, std::chrono::duration<double, std::milli>(delayMs),
            [] { return false; });
        if (stopToken.stop_requested()) {
            return res;
        }
    }
}
