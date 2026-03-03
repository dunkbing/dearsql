#include "application.hpp"
#include "database/async_helper.hpp"
#include "utils/logger.hpp"
#include "utils/sentry_init.hpp"

#include <cstdlib>
#include <exception>
#include <string>

namespace {
    [[noreturn]] void handleFatalException() {
        const std::exception_ptr current = std::current_exception();
        if (current) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                Logger::error(std::string("Unhandled exception: ") + e.what());
            } catch (...) {
                Logger::error("Unhandled exception: unknown");
            }
        } else {
            Logger::error("Unhandled exception: no active exception");
        }

        SentryInit::close();
        std::abort();
    }
} // namespace

int main() {
    SentryInit::initialize();

    std::set_terminate([] { handleFatalException(); });

    auto& app = Application::getInstance();

    if (!app.initialize()) {
        SentryInit::close();
        return -1;
    }

    app.run();
    app.cleanup();
    SentryInit::close();
    if (AsyncOperationControl::skipWaitOnDestroy().load(std::memory_order_relaxed)) {
        std::_Exit(0);
    }
    return 0;
}
