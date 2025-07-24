#include "platform/default_platform.hpp"
#include "application.hpp"
#include <iostream>

#ifdef USE_OPENGL_BACKEND
#include "imgui_impl_opengl3.h"
#endif

DefaultPlatform::DefaultPlatform(Application *app) : app_(app), window_(nullptr) {}

bool DefaultPlatform::initializePlatform(GLFWwindow *window) {
    window_ = window;
    std::cout << "Default platform initialized" << std::endl;
    return true;
}

bool DefaultPlatform::initializeImGuiBackend() {
#ifdef USE_OPENGL_BACKEND
    ImGui_ImplOpenGL3_Init("#version 330");
    std::cout << "ImGui OpenGL backend initialized" << std::endl;
#endif
    return true;
}

void DefaultPlatform::setupTitlebar() {
    // No-op for default platform
}

float DefaultPlatform::getTitlebarHeight() const {
    return 0.0f;
}

void DefaultPlatform::onConnectButtonClicked() {
    // This would typically be handled through UI menus on non-macOS platforms
    if (app_->getDatabaseSidebar()) {
        app_->getDatabaseSidebar()->showConnectionDialog();
    }
}

void DefaultPlatform::onSidebarToggleClicked() {
    // This would typically be handled through UI menus on non-macOS platforms
    app_->setSidebarVisible(!app_->isSidebarVisible());
}

void DefaultPlatform::cleanup() {
    // No-op for default platform
}

void DefaultPlatform::renderFrame() {
    // No-op for default platform - OpenGL rendering handled in main loop
}

void DefaultPlatform::shutdownImGui() {
#ifdef USE_OPENGL_BACKEND
    ImGui_ImplOpenGL3_Shutdown();
    std::cout << "ImGui OpenGL backend shutdown" << std::endl;
#endif
}
