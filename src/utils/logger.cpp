#include "utils/logger.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace {
    std::mutex& outputMutex() {
        static std::mutex mutex;
        return mutex;
    }

    const char* toString(const Logger::Level level) {
        switch (level) {
        case Logger::Level::Debug:
            return "DEBUG";
        case Logger::Level::Info:
            return "INFO";
        case Logger::Level::Warn:
            return "WARN";
        case Logger::Level::Error:
            return "ERROR";
        }
        return "UNKNOWN";
    }

    std::ostream& streamFor(const Logger::Level level) {
        if (level == Logger::Level::Error) {
            return std::cerr;
        }
        return std::cout;
    }

    std::string timestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto timeT = std::chrono::system_clock::to_time_t(now);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&timeT), "%H:%M:%S") << '.' << std::setfill('0')
            << std::setw(3) << ms.count();
        return oss.str();
    }
} // namespace

void Logger::log(const Level level, const std::string_view message) {
    std::scoped_lock lock(outputMutex());
    auto& out = streamFor(level);
    out << '[' << timestamp() << "] [" << toString(level) << "] " << message << std::endl;
}

void Logger::debug(const std::string_view message) {
    log(Level::Debug, message);
}

void Logger::info(const std::string_view message) {
    log(Level::Info, message);
}

void Logger::warn(const std::string_view message) {
    log(Level::Warn, message);
}

void Logger::error(const std::string_view message) {
    log(Level::Error, message);
}
