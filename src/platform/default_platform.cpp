#include "platform/default_platform.hpp"
#include "application.hpp"
#include <iostream>

#if defined(__linux__) || defined(_WIN32)
#include "imgui_impl_opengl3.h"
#endif

DefaultPlatform::DefaultPlatform(Application* app) : app_(app), window_(nullptr) {}

bool DefaultPlatform::initializePlatform(GLFWwindow* window) {
    window_ = window;
    std::cout << "Default platform initialized" << std::endl;
    return true;
}

bool DefaultPlatform::initializeImGuiBackend() {
#if defined(__linux__) || defined(_WIN32)
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

void DefaultPlatform::onSidebarToggleClicked() {
    // This would typically be handled through UI menus on non-macOS platforms
    app_->setSidebarVisible(!app_->isSidebarVisible());
}

void DefaultPlatform::cleanup() {
    // No-op for default platform
}

void DefaultPlatform::renderFrame() {
#if defined(_WIN32)
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    app_->renderMainUI();

    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(window_, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
#endif
}

void DefaultPlatform::shutdownImGui() {
#if defined(__linux__) || defined(_WIN32)
    ImGui_ImplOpenGL3_Shutdown();
    std::cout << "ImGui OpenGL backend shutdown" << std::endl;
#endif
}
