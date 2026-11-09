#include "platform/macos_platform.hpp"
#include "application.hpp"
#include <spdlog/spdlog.h>

MacOSPlatform::MacOSPlatform(Application* app, mydear::App& shell) : app_(app), shell_(shell) {}

MacOSPlatform::~MacOSPlatform() {
    cleanup();
}

void MacOSPlatform::setupTitlebar() {
    titlebar_ = std::make_unique<MacOSTitlebar>(app_, shell_.glfwWindow());
    titlebar_->setup();
}

float MacOSPlatform::getTitlebarHeight() const {
    return titlebar_ ? titlebar_->getHeight() : 0.0f;
}

void MacOSPlatform::onSidebarToggleClicked() {
    app_->setSidebarVisible(!app_->isSidebarVisible());
}

void MacOSPlatform::cleanup() {
    titlebar_.reset();
}

void MacOSPlatform::renderChrome() {
    if (titlebar_)
        titlebar_->render();
}

void MacOSPlatform::updateWorkspaceDropdown() {
    if (titlebar_)
        titlebar_->updateWorkspaceDropdown();
}

void MacOSPlatform::applyTheme(bool isDark) {
    if (titlebar_)
        titlebar_->applyTheme(isDark);
}
