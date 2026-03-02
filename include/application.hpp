#pragma once

#include "app_state.hpp"
#include "imgui.h"
#include "platform/platform_interface.hpp"
#include "themes.hpp"
#include "ui/db_sidebar_new.hpp"
#include "ui/tab_manager.hpp"
#include "utils/file_dialog.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__) || defined(_WIN32)
#include <GLFW/glfw3.h>
#endif

class Application {
public:
    static Application& getInstance();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // main app life-cycle
    bool initialize();
    void run();
    void cleanup();

    // Getters for managers and state
    [[nodiscard]] TabManager* getTabManager() const {
        return tabManager.get();
    }
    [[nodiscard]] DatabaseSidebarNew* getDatabaseSidebar() const {
        return databaseSidebar.get();
    }
    [[nodiscard]] FileDialog* getFileDialog() const {
        return fileDialog.get();
    }
    [[nodiscard]] AppState* getAppState() const {
        return appState.get();
    }

    // Theme management
    [[nodiscard]] bool isDarkTheme() const {
        return darkTheme;
    }
    void setDarkTheme(bool dark);
    [[nodiscard]] const Theme::Colors& getCurrentColors() const;

    // Selection state
    [[nodiscard]] std::shared_ptr<DatabaseInterface> getSelectedDatabase() const;
    void setSelectedDatabase(const std::shared_ptr<DatabaseInterface>& db);
    void clearSelectedDatabase();

    // UI state
    void resetDockingLayout() {
        dockingLayoutInitialized = false;
    }
    void dockTabToCenter(const std::string& tabName);

    // Database management
    std::vector<std::shared_ptr<DatabaseInterface>>& getDatabases() {
        return databases;
    }
    [[nodiscard]] const std::vector<std::shared_ptr<DatabaseInterface>>& getDatabases() const {
        return databases;
    }
    void addDatabase(const std::shared_ptr<DatabaseInterface>& db);
    void removeDatabase(const std::shared_ptr<DatabaseInterface>& db);
    void restorePreviousConnections();
    [[nodiscard]] std::size_t findDatabaseIndex(const std::shared_ptr<DatabaseInterface>& db) const;

    // Window reference (GLFW only on macOS/Windows)
#if defined(__APPLE__) || defined(_WIN32)
    [[nodiscard]] GLFWwindow* getWindow() const {
        return window;
    }
#endif

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

    // Workspace management
    [[nodiscard]] int getCurrentWorkspaceId() const {
        return currentWorkspaceId;
    }
    [[nodiscard]] std::string getCurrentWorkspaceName() const;
    void setCurrentWorkspace(int workspaceId);
    [[nodiscard]] std::vector<Workspace> getWorkspaces() const;
    int createWorkspace(const std::string& name, const std::string& description = "");
    bool deleteWorkspace(int workspaceId);
    void refreshWorkspaceConnections();

    // Platform-specific methods
#ifdef __APPLE__
    void onSidebarToggleClicked() const;
    [[nodiscard]] float getTitlebarHeight() const;
    void updateWorkspaceDropdown() const;
#endif
    [[nodiscard]] PlatformInterface* getPlatform() const {
        return platform_.get();
    }
    [[nodiscard]] bool isShutdownRequested() const;

private:
    Application() = default;
    ~Application() = default;

    // Core components
#if defined(__APPLE__) || defined(_WIN32)
    GLFWwindow* window = nullptr;
#endif
    std::unique_ptr<TabManager> tabManager;
    std::unique_ptr<DatabaseSidebarNew> databaseSidebar;
    std::unique_ptr<FileDialog> fileDialog;
    std::unique_ptr<AppState> appState;
    std::unique_ptr<PlatformInterface> platform_;

    // Application state
    bool darkTheme = true;
    bool sidebarVisible = true;
    float sidebarWidth = 0.25f;
    float targetSidebarWidth = 0.25f;
    float animationSpeed = 12.0f;
    ImGuiID leftDockId = 0;
    ImGuiID centerDockId = 0;
    ImGuiID rightDockId = 0;
    std::weak_ptr<DatabaseInterface> selectedDatabase;
    bool dockingLayoutInitialized = false;

    // Data
    std::vector<std::shared_ptr<DatabaseInterface>> databases;
    std::unordered_map<int, std::vector<std::shared_ptr<DatabaseInterface>>> workspaceDatabaseCache;
    int currentWorkspaceId = 1; // Default workspace

    // Private helper methods
#if defined(__APPLE__) || defined(_WIN32)
    bool initializeGLFW();
    bool initializeImGui() const;
#endif
    static void setupFonts();
    void setupDockingLayout(ImGuiID dockSpaceId);

public:
    void renderMainUI();

private:
    [[nodiscard]] bool hasPendingAsyncWork() const;
};
