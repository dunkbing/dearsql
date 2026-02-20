#include "utils/sentry_init.hpp"
#include "config.hpp"
#include <cstdlib>
#include <sentry.h>
#include <string>

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
} // namespace

void SentryInit::initialize() {
    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, SENTRY_DSN);
    sentry_options_set_release(options, APP_NAME "@" APP_VERSION);
    sentry_options_set_database_path(options, getSentryDbPath().c_str());
    sentry_init(options);
}

void SentryInit::close() {
    sentry_close();
}
