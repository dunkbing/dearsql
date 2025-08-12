#pragma once

#include "app_state.hpp"
#include "imgui.h"
#include "platform/platform_interface.hpp"
#include "themes.hpp"
#include "ui/db_sidebar.hpp"
#include "ui/tab_manager.hpp"
#include "utils/file_dialog.hpp"
#include <GLFW/glfw3.h>
#include <memory>
#include <vector>

class Application {
public:
    static Application &getInstance();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    // main app life-cycle
    bool initialize();
    void run() const;
    void cleanup();

    // Getters for managers and state
    [[nodiscard]] TabManager *getTabManager() const {
        return tabManager.get();
    }
    [[nodiscard]] DatabaseSidebar *getDatabaseSidebar() const {
        return databaseSidebar.get();
    }
    [[nodiscard]] FileDialog *getFileDialog() const {
        return fileDialog.get();
    }
    [[nodiscard]] AppState *getAppState() const {
        return appState.get();
    }

    // Theme management
    [[nodiscard]] bool isDarkTheme() const {
        return darkTheme;
    }
    void setDarkTheme(bool dark);
    [[nodiscard]] const Theme::Colors &getCurrentColors() const;

    // Selection state
    [[nodiscard]] int getSelectedDatabase() const {
        return selectedDatabase;
    }
    void setSelectedDatabase(const int index) {
        selectedDatabase = index;
    }
    [[nodiscard]] int getSelectedTable() const {
        return selectedTable;
    }
    void setSelectedTable(const int index) {
        selectedTable = index;
    }

    // UI state
    [[nodiscard]] bool isDockingLayoutInitialized() const {
        return dockingLayoutInitialized;
    }
    void setDockingLayoutInitialized(const bool initialized) {
        dockingLayoutInitialized = initialized;
    }
    void resetDockingLayout() {
        dockingLayoutInitialized = false;
    }

    // Database management
    std::vector<std::shared_ptr<DatabaseInterface>> &getDatabases() {
        return databases;
    }
    [[nodiscard]] const std::vector<std::shared_ptr<DatabaseInterface>> &getDatabases() const {
        return databases;
    }
    void addDatabase(const std::shared_ptr<DatabaseInterface> &db);
    void removeDatabase(int index);
    void restorePreviousConnections();

    // Window reference
    [[nodiscard]] GLFWwindow *getWindow() const {
        return window;
    }

    // Sidebar visibility
    [[nodiscard]] bool isSidebarVisible() const {
        return sidebarVisible;
    }
    void setSidebarVisible(const bool visible) {
        if (sidebarVisible != visible) {
            sidebarVisible = visible;
            targetSidebarWidth = visible ? 0.25f : 0.0f;
        }
    }

    // Log panel visibility
    [[nodiscard]] bool isLogPanelVisible() const {
        return logPanelVisible;
    }
    void setLogPanelVisible(const bool visible) {
        if (logPanelVisible != visible) {
            logPanelVisible = visible;
            targetLogPanelWidth = visible ? 0.25f : 0.0f;
        }
    }

    // Workspace management
    [[nodiscard]] int getCurrentWorkspaceId() const {
        return currentWorkspaceId;
    }
    [[nodiscard]] std::string getCurrentWorkspaceName() const;
    void setCurrentWorkspace(int workspaceId);
    [[nodiscard]] std::vector<Workspace> getWorkspaces() const;
    int createWorkspace(const std::string &name, const std::string &description = "");
    bool deleteWorkspace(int workspaceId);
    void refreshWorkspaceConnections();

    // Platform-specific methods
#ifdef USE_METAL_BACKEND
    void onSidebarToggleClicked() const;
    void onLogPanelToggleClicked() const;
    [[nodiscard]] float getTitlebarHeight() const;
    void updateWorkspaceDropdown() const;
#endif

private:
    Application() = default;
    ~Application() = default;

    // Core components
    GLFWwindow *window = nullptr;
    std::unique_ptr<TabManager> tabManager;
    std::unique_ptr<DatabaseSidebar> databaseSidebar;
    std::unique_ptr<FileDialog> fileDialog;
    std::unique_ptr<AppState> appState;
    std::unique_ptr<PlatformInterface> platform_;

    // Application state
    bool darkTheme = true;
    bool sidebarVisible = true;
    bool logPanelVisible = false;
    float sidebarWidth = 0.25f;
    float targetSidebarWidth = 0.25f;
    float logPanelWidth = 0.0f;
    float targetLogPanelWidth = 0.0f;
    float animationSpeed = 12.0f;
    ImGuiID leftDockId = 0;
    ImGuiID centerDockId = 0;
    ImGuiID rightDockId = 0;
    int selectedDatabase = -1;
    int selectedTable = -1;
    bool dockingLayoutInitialized = false;

    // Data
    std::vector<std::shared_ptr<DatabaseInterface>> databases;
    int currentWorkspaceId = 1; // Default workspace

    // Private helper methods
    bool initializeGLFW();
    bool initializeImGui() const;
    static void setupFonts();
    void setupDockingLayout(ImGuiID dockSpaceId);
    void renderMenuBar();

public:
    void renderMainUI();
};
