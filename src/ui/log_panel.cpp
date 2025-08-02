#include "ui/log_panel.hpp"
#include "imgui.h"
#include <iomanip>
#include <sstream>

LogPanel::LogPanel() {
    addLog(LogLevel::INFO, "Log panel initialized");
}

LogPanel &LogPanel::getInstance() {
    static LogPanel instance;
    return instance;
}

void LogPanel::render() {
    ImGui::Begin("Logs", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    // Header with controls
    if (ImGui::Button("Clear")) {
        clear();
    }
    ImGui::SameLine();

    ImGui::Checkbox("Timestamps", &showTimestamps_);
    ImGui::SameLine();

    // Filter level combo
    const char *levelNames[] = {"ALL", "DEBUG", "INFO", "WARN", "ERROR"};
    int currentLevel = filterLevel_ == LogLevel::ALL ? 0 : static_cast<int>(filterLevel_) + 1;
    ImGui::SetNextItemWidth(80);
    if (ImGui::Combo("##filter", &currentLevel, levelNames, 5)) {
        filterLevel_ = currentLevel == 0 ? LogLevel::ALL : static_cast<LogLevel>(currentLevel - 1);
    }

    ImGui::Separator();

    // Log content area
    ImGui::BeginChild("LogContent", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    for (const auto &entry : logs_) {
        // Apply filter (skip if filter level is higher than entry level, unless ALL is selected)
        if (filterLevel_ != LogLevel::ALL && entry.level < filterLevel_) {
            continue;
        }

        // Format log entry
        std::string logText;
        if (showTimestamps_) {
            logText += "[" + formatTimestamp(entry.timestamp) + "] ";
        }
        logText += "[" + std::string(getLevelString(entry.level)) + "] ";
        logText += entry.message;

        // Apply color based on log level
        ImVec4 color = getLevelColor(entry.level);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", logText.c_str());
        ImGui::PopStyleColor();
    }

    // Auto-scroll to bottom when new content is added
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}

void LogPanel::addLog(const LogLevel level, const std::string &message) {
    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.message = message;

    logs_.push_back(entry);

    // Limit log entries to prevent memory issues
    if (logs_.size() > 1000) {
        logs_.erase(logs_.begin(), logs_.begin() + 100);
    }
}

void LogPanel::clear() {
    logs_.clear();
    addLog(LogLevel::INFO, "Log cleared");
}

const char *LogPanel::getLevelString(const LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO ";
    case LogLevel::WARN:
        return "WARN ";
    case LogLevel::ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

ImVec4 LogPanel::getLevelColor(const LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG:
        return {0.7f, 0.7f, 0.7f, 1.0f}; // Gray
    case LogLevel::INFO:
        return {0.8f, 0.8f, 0.8f, 1.0f}; // Light gray
    case LogLevel::WARN:
        return {1.0f, 0.8f, 0.2f, 1.0f}; // Orange
    case LogLevel::ERROR:
        return {1.0f, 0.3f, 0.3f, 1.0f}; // Red
    default:
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
}

std::string LogPanel::formatTimestamp(const std::chrono::system_clock::time_point &timestamp) {
    const auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// Static utility functions
void LogPanel::debug(const std::string &message) {
    getInstance().addLog(LogLevel::DEBUG, message);
}

void LogPanel::info(const std::string &message) {
    getInstance().addLog(LogLevel::INFO, message);
}

void LogPanel::warn(const std::string &message) {
    getInstance().addLog(LogLevel::WARN, message);
}

void LogPanel::error(const std::string &message) {
    getInstance().addLog(LogLevel::ERROR, message);
}
