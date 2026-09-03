#include "acp/log.hpp"

#include <mutex>

namespace acp {

    namespace {
        std::mutex& loggerMutex() {
            static std::mutex m;
            return m;
        }
        LogFn& logger() {
            static LogFn fn;
            return fn;
        }
    } // namespace

    void setLogger(LogFn fn) {
        std::lock_guard lock(loggerMutex());
        logger() = std::move(fn);
    }

    void log(LogLevel level, const std::string& message) {
        LogFn fn;
        {
            std::lock_guard lock(loggerMutex());
            fn = logger();
        }
        if (fn) {
            fn(level, message);
        }
    }

} // namespace acp
