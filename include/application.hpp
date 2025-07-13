#pragma once

#include "imgui.h"
#include "imgui_impl_glfw.h"

#ifdef USE_METAL_BACKEND
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
// Forward declarations for Metal types (avoid Objective-C headers in C++)
#ifndef __OBJC__
typedef void *MetalDevice;
typedef void *MetalCommandQueue;
typedef void *MetalLayer;
#endif
#elif defined(USE_OPENGL_BACKEND)
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#endif
#include "app_state.hpp"
#include "tabs/tab_manager.hpp"
#include "ui/db_sidebar.hpp"
#include "utils/file_dialog.hpp"
#include <memory>
#include <vector>

class Application {
public:
    static Application &getInstance();

    // Delete copy constructor and assignment operator
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    // Main application lifecycle
    bool initialize();
    void run();
    void cleanup();

    // Getters for managers and state
    TabManager *getTabManager() const {
        return tabManager.get();
    }
    DatabaseSidebar *getDatabaseSidebar() const {
        return databaseSidebar.get();
    }
    FileDialog *getFileDialog() const {
        return fileDialog.get();
    }
    AppState *getAppState() const {
        return appState.get();
    }

    // Theme management
    bool isDarkTheme() const {
        return darkTheme;
    }
    void setDarkTheme(bool dark);

    // Selection state
    int getSelectedDatabase() const {
        return selectedDatabase;
    }
    void setSelectedDatabase(const int index) {
        selectedDatabase = index;
    }
    int getSelectedTable() const {
        return selectedTable;
    }
    void setSelectedTable(const int index) {
        selectedTable = index;
    }

    // UI state
    bool isDockingLayoutInitialized() const {
        return dockingLayoutInitialized;
    }
    void setDockingLayoutInitialized(bool initialized) {
        dockingLayoutInitialized = initialized;
    }

    // Database management
    std::vector<std::shared_ptr<DatabaseInterface>> &getDatabases() {
        return databases;
    }
    const std::vector<std::shared_ptr<DatabaseInterface>> &getDatabases() const {
        return databases;
    }
    void addDatabase(const std::shared_ptr<DatabaseInterface> &db);
    void restorePreviousConnections();

    // Window reference
    GLFWwindow *getWindow() const {
        return window;
    }

private:
    Application() = default;
    ~Application() = default;

    // Core components
    GLFWwindow *window = nullptr;
    std::unique_ptr<TabManager> tabManager;
    std::unique_ptr<DatabaseSidebar> databaseSidebar;
    std::unique_ptr<FileDialog> fileDialog;
    std::unique_ptr<AppState> appState;

#ifdef USE_METAL_BACKEND
// Metal-specific components (using void* for C++ compatibility)
#ifdef __OBJC__
    id metalDevice = nil;
    id metalCommandQueue = nil;
    id metalLayer = nil;
#else
    MetalDevice metalDevice = nullptr;
    MetalCommandQueue metalCommandQueue = nullptr;
    MetalLayer metalLayer = nullptr;
#endif
#endif

    // Application state
    bool darkTheme = true;
    int selectedDatabase = -1;
    int selectedTable = -1;
    bool dockingLayoutInitialized = false;

    // Data
    std::vector<std::shared_ptr<DatabaseInterface>> databases;

    // Private helper methods
    bool initializeGLFW();
    bool initializeImGui();
    static void setupFonts();
    void setupDockingLayout(ImGuiID dockSpaceId);
    void renderMainUI();
    void renderMenuBar();
};
