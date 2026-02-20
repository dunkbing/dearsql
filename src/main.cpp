#include "application.hpp"
#include "utils/logger.hpp"
#include "utils/sentry_init.hpp"

#include <cstdlib>
#include <exception>
#include <string>

namespace {
    [[noreturn]] void handleFatalException(const char* context) {
        std::exception_ptr current = std::current_exception();
        if (current) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                Logger::error(std::string(context) + ": " + e.what());
            } catch (...) {
                Logger::error(std::string(context) + ": unknown exception");
            }
        } else {
            Logger::error(std::string(context) + ": no active exception");
        }

        SentryInit::close();
        std::abort();
    }
} // namespace

int main() {
    SentryInit::initialize();

    std::set_terminate([] { handleFatalException("Unhandled exception reached std::terminate"); });

    try {
        auto& app = Application::getInstance();

        if (!app.initialize()) {
            SentryInit::close();
            return -1;
        }

        app.run();
        app.cleanup();
        SentryInit::close();
        return 0;
    } catch (...) {
        handleFatalException("Fatal exception in main");
    }
}
