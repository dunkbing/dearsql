#include "platform/macos_platform.hpp"
#include "application.hpp"
#include "imgui_impl_glfw.h"
#include "themes.hpp"
#include <iostream>
#define GLFW_EXPOSE_NATIVE_COCOA
#import <AppKit/AppKit.h>
#import <GLFW/glfw3native.h>
#import <objc/runtime.h>

MacOSPlatform::MacOSPlatform(Application* app) : app_(app) {}

MacOSPlatform::~MacOSPlatform() {
    cleanup();
}

bool MacOSPlatform::initializePlatform(GLFWwindow* window) {
    window_ = window;

    backend_ = std::make_unique<MacOSMetalBackend>(window_);
    if (!backend_->metalDevice_) {
        std::cerr << "Failed to initialize Metal backend" << std::endl;
        return false;
    }

    // drag-and-drop: open supported files dropped onto the window
    glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
        for (int i = 0; i < count; i++) {
            Application::getInstance().openFile(std::string(paths[i]));
        }
        glfwFocusWindow(w);
    });

    // "Open With" / double-click in Finder: intercept application:openFile:
    Class appDelegateClass = [NSApp.delegate class];
    SEL openFileSel = @selector(application:openFile:);
    if (![appDelegateClass instancesRespondToSelector:openFileSel]) {
        IMP openFileImp =
            imp_implementationWithBlock(^BOOL(id, NSApplication*, NSString* filename) {
              std::string path([filename UTF8String]);
              dispatch_async(dispatch_get_main_queue(), ^{
                Application::getInstance().openFile(path);
              });
              return YES;
            });
        class_addMethod(appDelegateClass, openFileSel, openFileImp, "c@:@@");
    }

    return true;
}

bool MacOSPlatform::initializeImGuiBackend() {
    ImGui_ImplGlfw_InitForOther(window_, true);
    bool ok = backend_->initializeImGui();
    std::cout << "ImGui Metal backend initialized" << std::endl;
    return ok;
}

void MacOSPlatform::setupTitlebar() {
    titlebar_ = std::make_unique<MacOSTitlebar>(app_, window_);
    titlebar_->setup();
}

float MacOSPlatform::getTitlebarHeight() const {
    if (titlebar_) {
        return titlebar_->getHeight();
    }
    return 0.0f;
}

void MacOSPlatform::onSidebarToggleClicked() {
    std::cout << "Sidebar toggle clicked" << std::endl;
    try {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    } catch (const std::exception& e) {
        std::cerr << "Exception in onSidebarToggleClicked: " << e.what() << std::endl;
    }
}

void MacOSPlatform::cleanup() {
    titlebar_.reset();
    backend_.reset();
    window_ = nullptr;
}

void MacOSPlatform::renderFrame() {
    @autoreleasepool {
        const auto& clearCol =
            app_->isDarkTheme() ? Theme::NATIVE_DARK.base : Theme::NATIVE_LIGHT.base;
        ImVec4 clearColor(clearCol.x, clearCol.y, clearCol.z, clearCol.w);

        backend_->beginFrame(clearColor);
        ImGui::NewFrame();

        titlebar_->render();
        app_->renderMainUI();

        ImGui::Render();

        backend_->renderDrawData(ImGui::GetDrawData());
        backend_->present();
    }
}

void MacOSPlatform::shutdownImGui() {
    backend_->shutdown();
    ImGui_ImplGlfw_Shutdown();
    std::cout << "ImGui Metal backend shutdown" << std::endl;
}

void MacOSPlatform::updateWorkspaceDropdown() {
    if (titlebar_) {
        titlebar_->updateWorkspaceDropdown();
    }
}

ImTextureID MacOSPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    return backend_->createTextureFromRGBA(pixels, width, height);
}
