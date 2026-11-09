#include "platform/shell_platform.hpp"
#include "application.hpp"
#include <spdlog/spdlog.h>

ShellPlatform::ShellPlatform(Application* app, mydear::App& shell) : app_(app), shell_(shell) {}

ShellPlatform::~ShellPlatform() {
    cleanup();
}

void ShellPlatform::setupTitlebar() {
#ifdef __linux__
    titlebar_ =
        std::make_unique<ShellTitlebar>(app_, static_cast<GtkWidget*>(shell_.nativeWindow()));
#else
    titlebar_ = std::make_unique<ShellTitlebar>(app_, shell_.glfwWindow());
#endif
    titlebar_->setup();
}

float ShellPlatform::getTitlebarHeight() const {
    return titlebar_ ? titlebar_->getHeight() : 0.0f;
}

void ShellPlatform::onSidebarToggleClicked() {
    app_->setSidebarVisible(!app_->isSidebarVisible());
}

void ShellPlatform::cleanup() {
    titlebar_.reset();
}

void ShellPlatform::renderChrome() {
    if (titlebar_)
        titlebar_->render();
}

void ShellPlatform::updateWorkspaceDropdown() {
    if (titlebar_)
        titlebar_->updateWorkspaceDropdown();
}

void ShellPlatform::applyTheme(bool isDark) {
    if (titlebar_)
        titlebar_->applyTheme(isDark);
}
