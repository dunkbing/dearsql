#pragma once

#include <string_view>

namespace Logger {
    enum class Level { Debug, Info, Warn, Error };

    void log(Level level, std::string_view message);

    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);
} // namespace Logger
