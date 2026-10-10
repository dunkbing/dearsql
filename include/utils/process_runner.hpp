#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

struct ProcessResult {
    bool success = false;
    int exitCode = -1;
    std::string output;
    std::string errorMessage;
    // stopped by the stop token or the timeout
    bool cancelled = false;
};

struct ProcessSpec {
    std::vector<std::string> args;
    // added to the inherited environment; PATH lookup still uses this process's PATH
    std::unordered_map<std::string, std::string> environment;
    // 0 = no limit
    std::chrono::milliseconds timeout{0};
};

class ProcessRunner {
public:
    // runs to exit, capturing stdout+stderr; stdin is /dev/null. a stop request or the
    // timeout sends SIGTERM to the child's process group, then SIGKILL after a short grace
    static ProcessResult run(const ProcessSpec& spec, std::stop_token stop = {});
};
