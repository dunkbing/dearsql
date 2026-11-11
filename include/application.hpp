#pragma once

#include "app_state.hpp"
#include "imgui.h"
#include "imshell/app.hpp"
#include "themes.hpp"
#include "ui/db_sidebar.hpp"
#include "ui/tab_manager.hpp"
#include "utils/file_dialog.hpp"
#include <memory>
#if (defined(__APPLE__) || defined(_WIN32)) && !defined(DEARSQL_MOBILE)
#include <GLFW/glfw3.h>
#endif
#include <unordered_map>
#include <vector>

#if (defined(__APPLE__) || defined(_WIN32)) && !defined(DEARSQL_MOBILE)
#include <GLFW/glfw3.h>
#endif

// the app is imshell's delegate: imshell owns the window, backend and loop
class Application : public imshell::Delegate {
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
        return theme_->dark;
    }
    [[nodiscard]] const Theme::ThemeDef& getTheme() const {
        return *theme_;
    }
    // by Theme::THEMES id; applied at once and saved
    void setTheme(std::string_view id);
    [[nodiscard]] const Theme::Colors& getCurrentColors() const;

    [[nodiscard]] float getFontScale() const {
        return fontScale_;
    }
    void setFontScale(float scale);
    [[nodiscard]] bool showFps() const {
        return showFps_;
    }
    // width the bottom-right fps counter takes, for content right-aligned at the bottom
    [[nodiscard]] float fpsCounterWidth() const;
    [[nodiscard]] bool vsync() const {
        return vsync_;
    }
    // frames per second without vsync; 0 = no limit
    [[nodiscard]] int maxFps() const {
        return maxFps_;
    }
    void setShowFps(bool on);
    void setVSync(bool on);
    void setMaxFps(int fps);

    // Selection state
    [[nodiscard]] std::shared_ptr<DatabaseInterface> getSelectedDatabase() const;
    void setSelectedDatabase(const std::shared_ptr<DatabaseInterface>& db);
    void clearSelectedDatabase();

    // UI state
    void resetDockingLayout() {
        dockingLayoutInitialized = false;
    }
    void dockTabToCenter(const std::string& tabName) const;

    // Open a file by path — routes to the right handler by extension
    void openFile(const std::string& path) override;

    // imshell::Delegate
    void setup() override;
    void frame() override;
    bool wantsFrames() override;
    bool shouldQuit() override;

    // Database management
    std::vector<std::shared_ptr<DatabaseInterface>>& getDatabases() {
        return databases;
    }
    [[nodiscard]] const std::vector<std::shared_ptr<DatabaseInterface>>& getDatabases() const {
        return databases;
    }
    void addDatabase(const std::shared_ptr<DatabaseInterface>& db);
    void removeDatabase(const std::shared_ptr<DatabaseInterface>& db);
    // another device or process changed the saved connections: add, drop and rename
    // sidebar rows to match, leaving open connections alone
    void syncSavedConnections();
    void restorePreviousConnections();
    [[nodiscard]] std::size_t findDatabaseIndex(const std::shared_ptr<DatabaseInterface>& db) const;

    // License-aware connection/workspace helpers
    // Returns new connection ID, or -1 on DB error, or -2 if the free-tier limit (3) is reached.
    int saveConnection(const SavedConnection& conn);
    [[nodiscard]] bool canAddConnection() const;
    [[nodiscard]] bool canAddWorkspace() const;

    // the os window: NSWindow*, UIWindow*, HWND or GtkWindow*
    [[nodiscard]] void* nativeWindow() const {
        return shell_.nativeWindow();
    }
    // Window reference (GLFW only on macOS/Windows)
#if (defined(__APPLE__) || defined(_WIN32)) && !defined(DEARSQL_MOBILE)
    [[nodiscard]] GLFWwindow* getWindow() const {
        return window;
    }
#endif

    // Sidebar visibility
    [[nodiscard]] bool isSidebarVisible() const {
        return sidebarVisible;
    }
    void setSidebarVisible(const bool visible) {
        sidebarVisible = visible;
    }

    // fraction of the window the sidebar takes when visible
    static constexpr float kSidebarWidth = 0.28f;

    // Workspace management
    [[nodiscard]] int getCurrentWorkspaceId() const {
        return currentWorkspaceId;
    }
    [[nodiscard]] std::string getCurrentWorkspaceName() const;
    void setCurrentWorkspace(int workspaceId);
    [[nodiscard]] std::vector<Workspace> getWorkspaces() const;
    int createWorkspace(const std::string& name, const std::string& description = "");
    bool deleteWorkspace(int workspaceId);
    bool renameWorkspace(int workspaceId, const std::string& name);
    void refreshWorkspaceConnections();

    // window or uikit view, graphics, frame loop
    [[nodiscard]] imshell::App& shell() {
        return shell_;
    }
    [[nodiscard]] bool isShutdownRequested() const;

private:
    Application() = default;
    ~Application() override = default;

    // Core components
#if (defined(__APPLE__) || defined(_WIN32)) && !defined(DEARSQL_MOBILE)
    GLFWwindow* window = nullptr;
#endif
    imshell::App shell_; // window or uikit view, metal, imgui backends, frame loop
    std::unique_ptr<TabManager> tabManager;
    std::unique_ptr<DatabaseSidebarNew> databaseSidebar;
    std::unique_ptr<FileDialog> fileDialog;
    std::unique_ptr<AppState> appState;

    // Application state
    const Theme::ThemeDef* theme_ = &Theme::THEMES[0];
    float fontScale_ = 1.0f;
    bool showFps_ = false;
    bool vsync_ = true;
    int maxFps_ = 120;
    bool sidebarVisible = true;
    // previous frame's value: a change means the dock layout has to be rebuilt
    bool sidebarVisibleLastFrame_ = true;
    // phone-width screens: the sidebar is a tab next to the open tabs, not a split
    bool compactLayout_ = false;
    int focusDatabases_ = 0; // frames left: a window appearing on the first frame takes focus
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
    void setupDockingLayout(ImGuiID dockSpaceId);

public:
    void renderMainUI();
    // The dock node holding the tabs, i.e. everything but the sidebar. Zero before
    // the layout is built or while the sidebar is hidden.
    [[nodiscard]] ImGuiID getCenterDockId() const {
        return centerDockId;
    }

private:
    [[nodiscard]] bool hasPendingAsyncWork() const;
};
