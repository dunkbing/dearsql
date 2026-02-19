#include "utils/sentry_init.hpp"
#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <sentry.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {
    std::string getSentryDbPath() {
#ifdef __APPLE__
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + "/Library/Application Support/DearSQL/.sentry-native";
        }
#elif defined(__linux__)
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg) {
            return std::string(xdg) + "/DearSQL/.sentry-native";
        }
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + "/.local/share/DearSQL/.sentry-native";
        }
#endif
        return ".sentry-native";
    }

    std::string getCrashpadHandlerPath() {
        namespace fs = std::filesystem;
#ifdef __APPLE__
        char buf[PATH_MAX];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            auto handler = fs::path(buf).parent_path() / "crashpad_handler";
            if (fs::exists(handler)) {
                return handler.string();
            }
        }
#elif defined(__linux__)
        auto handler = fs::read_symlink("/proc/self/exe").parent_path() / "crashpad_handler";
        if (fs::exists(handler)) {
            return handler.string();
        }
#endif
        return "crashpad_handler";
    }
} // namespace

void SentryInit::initialize() {
    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, SENTRY_DSN);
    sentry_options_set_release(options, APP_NAME "@" APP_VERSION);
    sentry_options_set_database_path(options, getSentryDbPath().c_str());
    sentry_options_set_handler_path(options, getCrashpadHandlerPath().c_str());
    sentry_init(options);
}

void SentryInit::close() {
    sentry_close();
}
