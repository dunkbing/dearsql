#include "application.hpp"
#include "utils/sentry_init.hpp"

int main() {
    SentryInit::initialize();

    auto& app = Application::getInstance();

    if (!app.initialize()) {
        SentryInit::close();
        return -1;
    }

    app.run();
    app.cleanup();

    SentryInit::close();
    return 0;
}
