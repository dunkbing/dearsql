#include "application.hpp"

int main() {
    auto& app = Application::getInstance();

    if (!app.initialize()) {
        return -1;
    }

    app.run();
    app.cleanup();

    return 0;
}
