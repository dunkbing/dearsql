#pragma once

#include <functional>
#include <string>

namespace acp {

    enum class LogLevel { Debug, Info, Warn, Error };
    using LogFn = std::function<void(LogLevel, const std::string&)>;

    // the library is silent unless a sink is installed
    void setLogger(LogFn fn);
    void log(LogLevel level, const std::string& message);

} // namespace acp
