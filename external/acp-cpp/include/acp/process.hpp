#pragma once

#include <string>
#include <vector>

// Small blocking process helper used for install commands and archive tools.
// Output is stdout and stderr merged.
namespace acp {

    struct RunResult {
        bool ok = false; // launched and exited normally
        int exitCode = -1;
        std::string output;
        std::string error; // launch failure, when !ok
    };

    RunResult run(const std::vector<std::string>& argv);

} // namespace acp
