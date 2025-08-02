#pragma once

#include "imgui.h"
#include <chrono>
#include <string>
#include <vector>

enum class LogLevel { ALL = -1, DEBUG = 0, INFO, WARN, ERROR };

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string message;
};

class LogPanel {
public:
    LogPanel();
    ~LogPanel() = default;

    void render();
    void addLog(LogLevel level, const std::string &message);
    void clear();

    // Utility functions for easy logging
    static void debug(const std::string &message);
    static void info(const std::string &message);
    static void warn(const std::string &message);
    static void error(const std::string &message);

    static LogPanel &getInstance();

private:
    std::vector<LogEntry> logs_;
    bool showTimestamps_ = true;
    LogLevel filterLevel_ = LogLevel::ALL;

    const char *getLevelString(LogLevel level) const;
    ImVec4 getLevelColor(LogLevel level) const;
    std::string formatTimestamp(const std::chrono::system_clock::time_point &timestamp) const;
};
