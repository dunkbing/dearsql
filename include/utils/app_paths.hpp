#pragma once

#include <cstdlib>
#include <filesystem>

// Everything DearSQL keeps on disk lives under one directory. Resolve it here so
// call sites stop repeating the HOME/USERPROFILE dance.
namespace AppPaths {

    // ~/.dearsql, or ./ when the home directory cannot be resolved
    inline std::filesystem::path dataDir() {
#if defined(_WIN32)
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        return home && *home ? std::filesystem::path(home) / ".dearsql"
                             : std::filesystem::path(".");
    }

    // subdirectory under the data dir, created on demand; empty path on failure
    inline std::filesystem::path ensureSubdir(const std::string& name) {
        const std::filesystem::path dir = dataDir() / name;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return ec ? std::filesystem::path{} : dir;
    }

} // namespace AppPaths
