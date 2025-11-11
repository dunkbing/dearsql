#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "imgui_impl_glfw.h"
#include "platform/default_platform.hpp"
#include "platform/macos_platform.hpp"
#include "themes.hpp"
#include "utils/file_dialog.hpp"
#include "utils/logger.hpp"
#include "utils/toggle_button.hpp"
#include <algorithm>
#include <csignal>
#include <format>
#include <imgui_internal.h>
#include <iostream>
#include <limits>

#if defined(__linux__) || defined(_WIN32)
#include "imgui_impl_opengl3.h"
#endif

#include "embedded_fonts.hpp"

static void signal_handler(const int signal) {
    if (signal == SIGTERM || signal == SIGINT) {
        Application::getInstance().cleanup();
        exit(0);
    }
}

Application& Application::getInstance() {
    static Application instance;
    return instance;
}

namespace {
    constexpr double kIdleActivationDelaySeconds = 0.25; // time after last activity before idling
    constexpr double kMinimumWaitSeconds = 1.0 / 120.0;  // keep responsive when active
    constexpr double kMaximumWaitSeconds = 0.2;          // cap sleep to keep UI responsive

    bool isImGuiUserActive() {
        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsAnyItemActive() || ImGui::IsAnyItemFocused() || ImGui::IsAnyItemHovered()) {
            return true;
        }

        if (ImGuiContext* ctx = ImGui::GetCurrentContext()) {
            if (ctx->MovingWindow != nullptr || ctx->DragDropActive) {
                return true;
            }

            if (ctx->LastActiveId != 0 && ctx->LastActiveIdTimer < 0.05f) {
                return true;
            }
        }

        const ImVec2 mouseDelta = io.MouseDelta;
        if (mouseDelta.x != 0.0f || mouseDelta.y != 0.0f || io.MouseWheel != 0.0f ||
            io.MouseWheelH != 0.0f) {
            return true;
        }

        for (bool down : io.MouseDown) {
            if (down) {
                return true;
            }
        }

        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
            const ImGuiKeyData* keyData = ImGui::GetKeyData(static_cast<ImGuiKey>(key));
            if (keyData != nullptr && (keyData->Down || keyData->DownDuration == 0.0f)) {
                return true;
            }
        }

        if (io.InputQueueCharacters.Size > 0) {
            return true;
        }

        return false;
    }
} // namespace

bool Application::initialize() {
    std::cout << "Starting DearSQL..." << std::endl;

    if (!initializeGLFW()) {
        return false;
    }

    // Initialize platform-specific components
#ifdef __APPLE__
    platform_ = std::make_unique<MacOSPlatform>(this);
#else
    platform_ = std::make_unique<DefaultPlatform>(this);
#endif

    if (!platform_->initializePlatform(window)) {
        std::cerr << "Failed to initialize platform" << std::endl;
        return false;
    }

    if (!initializeImGui()) {
        return false;
    }

    // Initialize NFD
    if (!FileDialog::initialize()) {
        std::cerr << "Failed to initialize Native File Dialog" << std::endl;
        return false;
    }

    // Create managers
    tabManager = std::make_unique<TabManager>();
    databaseSidebar = std::make_unique<DatabaseSidebarNew>();
    fileDialog = std::make_unique<FileDialog>();

    // Initialize app state
    appState = std::make_unique<AppState>();
    if (!appState->initialize()) {
        std::cerr << "Failed to initialize app state" << std::endl;
        return false;
    }

    // Restore current workspace from settings
    const std::string workspaceIdStr = appState->getSetting("current_workspace", "1");
    currentWorkspaceId = std::stoi(workspaceIdStr);

    // Restore previous connections for current workspace
    restorePreviousConnections();

    // Register signal handler
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Setup title bar after window creation
    platform_->setupTitlebar();

#ifdef __APPLE__
    // Update workspace dropdown after titlebar is set up
    updateWorkspaceDropdown();
#endif

#ifdef __APPLE__
    std::cout << "Application initialized successfully (with Metal backend)" << std::endl;
#else
    std::cout << "Application initialized successfully (with OpenGL backend)" << std::endl;
#endif
    return true;
}

void Application::run() {
#if defined(__linux__) || defined(_WIN32)
    glClearColor(darkTheme ? 0.110f : 0.957f, darkTheme ? 0.110f : 0.957f,
                 darkTheme ? 0.137f : 0.957f, 0.98f);
#endif

    double lastInteractionTime = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        const double frameStart = glfwGetTime();
        const double timeSinceInteraction = frameStart - lastInteractionTime;
        const bool hadAsyncWork = hasPendingAsyncWork();

        double waitTimeout = 0.0;
        // Only enter power-save mode when there has been no recent interaction and no async work
        const bool allowIdle =
            (timeSinceInteraction >= kIdleActivationDelaySeconds) && !hadAsyncWork;
        if (allowIdle) {
            const double idleTime =
                std::max(0.0, timeSinceInteraction - kIdleActivationDelaySeconds);
            waitTimeout = std::clamp(idleTime, kMinimumWaitSeconds, kMaximumWaitSeconds);
        }

        if (waitTimeout > 0.0) {
            glfwWaitEventsTimeout(waitTimeout);
        } else {
            glfwPollEvents();
        }

#ifdef __APPLE__
        platform_->renderFrame();
#elif defined(__linux__) || defined(_WIN32)
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderMainUI();

        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
#endif

        const bool userActive = isImGuiUserActive();
        const bool hasAsyncWork = hasPendingAsyncWork();

        if (userActive || hasAsyncWork) {
            lastInteractionTime = glfwGetTime();
        }
    }
}

void Application::cleanup() {
    std::cout << "Cleaning up DearSQL..." << std::endl;

    // Cleanup databases
    for (auto& db : databases) {
        if (db) {
            db->disconnect();
        }
    }
    databases.clear();
    std::cout << "Databases disconnected" << std::endl;

    // Cleanup components
    tabManager.reset();
    databaseSidebar.reset();
    fileDialog.reset();
    std::cout << "Components cleaned up" << std::endl;

    // Cleanup NFD
    FileDialog::cleanup();
    std::cout << "File dialog cleaned up" << std::endl;

    if (platform_) {
        platform_->shutdownImGui();
        platform_->cleanup();
        platform_.reset();
    }
    ImGui_ImplGlfw_Shutdown();
    std::cout << "ImGui GLFW backend shutdown" << std::endl;
    ImGui::DestroyContext();
    std::cout << "ImGui context destroyed" << std::endl;

    if (window) {
        glfwDestroyWindow(window);
    }
    glfwTerminate();

    std::cout << "Application cleanup completed" << std::endl;
}

void Application::setDarkTheme(const bool dark) {
    darkTheme = dark;
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);
}

bool Application::hasPendingAsyncWork() const {
    return std::ranges::any_of(databases, [](const std::shared_ptr<DatabaseInterface>& db) {
        if (!db) {
            return false;
        }

        if (db->isConnecting() || db->isLoadingTables() || db->isLoadingViews() ||
            db->isLoadingSequences()) {
            return true;
        }

        if (const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db)) {
            if (pgDb->isLoadingSchemas() || pgDb->isLoadingDatabases() ||
                pgDb->isLoadingSequences()) {
                return true;
            }
        }

        if (const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db)) {
            if (mysqlDb->isLoadingDatabases()) {
                return true;
            }
        }

        if (const auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(db)) {
            if (sqliteDb->isLoadingTables() || sqliteDb->isLoadingViews()) {
                return true;
            }
        }

        if (const auto redisDb = std::dynamic_pointer_cast<RedisDatabase>(db)) {
            if (redisDb->isLoadingTables()) {
                return true;
            }
        }

        return false;
    });
}

const Theme::Colors& Application::getCurrentColors() const {
    return darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
}

std::shared_ptr<DatabaseInterface> Application::getSelectedDatabase() const {
    return selectedDatabase.lock();
}

void Application::setSelectedDatabase(const std::shared_ptr<DatabaseInterface>& db) {
    selectedDatabase = db;
}

void Application::clearSelectedDatabase() {
    selectedDatabase.reset();
}

void Application::addDatabase(const std::shared_ptr<DatabaseInterface>& db) {
    databases.push_back(db);
}

void Application::removeDatabase(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        return;
    }

    const auto it = std::ranges::find(databases, db);
    if (it == databases.end()) {
        return;
    }

    if (*it) {
        (*it)->disconnect();
    }

    databases.erase(it);

    if (auto selected = selectedDatabase.lock(); selected && selected == db) {
        selectedDatabase.reset();
    }
}

std::size_t Application::findDatabaseIndex(const std::shared_ptr<DatabaseInterface>& db) const {
    const auto it = std::ranges::find(databases, db);
    if (it == databases.end()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(std::distance(databases.begin(), it));
}

void Application::restorePreviousConnections() {
    if (!appState) {
        return;
    }

    const auto savedConnections = appState->getConnectionsForWorkspace(currentWorkspaceId);
    Logger::info(
        std::format("Restoring {} connections for current workspace", savedConnections.size()));

    for (const auto& conn : savedConnections) {
        std::shared_ptr<DatabaseInterface> db = nullptr;

        if (conn.connectionInfo.type == DatabaseType::POSTGRESQL) {
            db = std::make_shared<PostgresDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::MYSQL) {
            // Use 'mysql' as default database if none specified (needed for connection)
            DatabaseConnectionInfo info = conn.connectionInfo;
            if (info.database.empty()) {
                info.database = "mysql";
            }
            db = std::make_shared<MySQLDatabase>(info);
        } else if (conn.connectionInfo.type == DatabaseType::SQLITE) {
            db = std::make_shared<SQLiteDatabase>(conn.connectionInfo.name,
                                                  conn.connectionInfo.path);
        } else if (conn.connectionInfo.type == DatabaseType::REDIS) {
            db = std::make_shared<RedisDatabase>(
                conn.connectionInfo.name, conn.connectionInfo.host, conn.connectionInfo.port,
                conn.connectionInfo.password, conn.connectionInfo.username);
        }

        if (db) {
            // Store the saved connection ID in the database instance
            db->setConnectionId(conn.id);
            Logger::debug(std::format("Added connection (will connect when expanded): {}",
                                      conn.connectionInfo.name));
            databases.push_back(db);
        }
    }
}

bool Application::initializeGLFW() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }

#ifdef __APPLE__
    // Metal backend doesn't need OpenGL context hints
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#elif defined(__linux__) || defined(_WIN32)
    // OpenGL backend requires context hints
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

#ifdef NDEBUG
    const auto title = "DearSQL";
#else
    const auto title = "";
#endif
    window = glfwCreateWindow(1280, 720, title, nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

#if defined(__linux__) || defined(_WIN32)
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
#endif

    std::cout << "GLFW window created successfully" << std::endl;
    return true;
}

bool Application::initializeImGui() const {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    setupFonts();

    ImGui::StyleColorsDark();
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);

    // Initialize GLFW backend
#ifdef __APPLE__
    ImGui_ImplGlfw_InitForOther(window, true);
#elif defined(__linux__) || defined(_WIN32)
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#endif

    // Initialize platform-specific ImGui backend
    if (!platform_->initializeImGuiBackend()) {
        std::cerr << "Failed to initialize ImGui backend" << std::endl;
        return false;
    }

#ifdef __APPLE__
    std::cout << "ImGui initialized with Metal backend" << std::endl;
#elif defined(__linux__) || defined(_WIN32)
    std::cout << "ImGui initialized with OpenGL backend" << std::endl;
#endif

    return true;
}

void Application::setupFonts() {
    const ImGuiIO& io = ImGui::GetIO();

    // load embedded fonts first
    const size_t embeddedFontCount = getEmbeddedFontCount();
    if (embeddedFontCount > 0) {
        ImFontConfig fontConfig;
        const EmbeddedFont* embeddedFonts = getEmbeddedFonts();
        for (size_t i = 0; i < embeddedFontCount; ++i) {
            const EmbeddedFont& font = embeddedFonts[i];

            // Choose appropriate glyph ranges based on font name
            const ImWchar* ranges = nullptr;
            std::string fontName = font.name;

            if (fontName.find("Cyrillic") != std::string::npos) {
                ranges = io.Fonts->GetGlyphRangesCyrillic();
            } else {
                ranges = io.Fonts->GetGlyphRangesDefault();
            }

            // Create a copy of fontConfig for each font to avoid reuse issues
            ImFontConfig embeddedFontConfig = fontConfig;
            // Don't let ImGui free the embedded data
            embeddedFontConfig.FontDataOwnedByAtlas = false;

            const ImFont* loadedFont = io.Fonts->AddFontFromMemoryTTF(
                (void*)font.data, static_cast<int>(font.size), 16.0f, &embeddedFontConfig, ranges);
            if (!fontConfig.MergeMode) {
                fontConfig.MergeMode = true;
            }

            if (loadedFont) {
                std::cout << "✓ Successfully loaded embedded font: " << font.name << std::endl;
            }
        }
    }

    // Build the font atlas only for OpenGL backend
    // Metal backend handles this automatically
#if defined(__linux__) || defined(_WIN32)
    io.Fonts->Build();
#endif
}

void Application::setCurrentWorkspace(const int workspaceId) {
    if (currentWorkspaceId == workspaceId) {
        return;
    }

    currentWorkspaceId = workspaceId;

    // Save current workspace to settings
    if (appState) {
        appState->setSetting("current_workspace", std::to_string(currentWorkspaceId));
        appState->updateWorkspaceLastUsed(currentWorkspaceId);
    }

    // Refresh connections for new workspace
    refreshWorkspaceConnections();
}

std::vector<Workspace> Application::getWorkspaces() const {
    if (!appState) {
        return {};
    }
    return appState->getWorkspaces();
}

std::string Application::getCurrentWorkspaceName() const {
    if (!appState) {
        return "Default";
    }

    auto workspaces = appState->getWorkspaces();
    for (const auto& workspace : workspaces) {
        if (workspace.id == currentWorkspaceId) {
            return workspace.name;
        }
    }

    return "Default"; // Fallback
}

int Application::createWorkspace(const std::string& name, const std::string& description) {
    if (!appState) {
        return -1;
    }

    Workspace workspace;
    workspace.name = name;
    workspace.description = description;

    const int newWorkspaceId = appState->saveWorkspace(workspace);

    if (newWorkspaceId > 0) {
#ifdef __APPLE__
        updateWorkspaceDropdown();
#endif
        // Switch to the newly created workspace
        setCurrentWorkspace(newWorkspaceId);
    }

    return newWorkspaceId;
}

bool Application::deleteWorkspace(const int workspaceId) {
    if (!appState || workspaceId == 1) { // Can't delete default workspace
        return false;
    }

    bool success = appState->deleteWorkspace(workspaceId);

    // If we deleted the current workspace, switch to default
    if (success && currentWorkspaceId == workspaceId) {
        setCurrentWorkspace(1);
    }

    return success;
}

void Application::refreshWorkspaceConnections() {
    // Clear current connections
    for (auto& db : databases) {
        if (db) {
            db->disconnect();
        }
    }
    databases.clear();

    // Restore connections for current workspace
    restorePreviousConnections();

    // Reset UI state
    clearSelectedDatabase();
    resetDockingLayout();
}

void Application::setupDockingLayout(const ImGuiID dockSpaceId) {
    if (dockingLayoutInitialized)
        return;

    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, ImGui::GetMainViewport()->Size);

    // Check if we should use docking for sidebar
    const bool shouldUseSidebar = targetSidebarWidth > 0.01f;

    if (shouldUseSidebar) {
        // Two-panel layout: sidebar | center
        ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Left, sidebarWidth, &leftDockId,
                                    &centerDockId);

        ImGui::DockBuilderGetNode(leftDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;

        ImGui::DockBuilderDockWindow("Databases", leftDockId);
        ImGui::DockBuilderDockWindow(getCurrentWorkspaceName().c_str(), centerDockId);

        for (const auto& tab : tabManager->getTabs()) {
            ImGui::DockBuilderDockWindow(tab->getName().c_str(), centerDockId);
        }

        rightDockId = 0;
    } else {
        // Single panel layout: just center
        ImGui::DockBuilderDockWindow(getCurrentWorkspaceName().c_str(), dockSpaceId);

        for (const auto& tab : tabManager->getTabs()) {
            ImGui::DockBuilderDockWindow(tab->getName().c_str(), dockSpaceId);
        }

        leftDockId = 0;
        centerDockId = 0;
        rightDockId = 0;
    }

    ImGui::DockBuilderFinish(dockSpaceId);
    dockingLayoutInitialized = true;
}

void Application::renderMainUI() {
    // DockSpace setup
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Direct show/hide sidebar without animation
    sidebarWidth = targetSidebarWidth;

    // Always use docking when sidebar is visible for proper resizing
    const bool shouldUseDocking = targetSidebarWidth > 0.01f;

    // Rebuild layout when sidebar visibility changes
    static bool lastSidebarVisible = false;
    const bool currentSidebarVisible = targetSidebarWidth > 0.01f;
    if (lastSidebarVisible != currentSidebarVisible) {
        dockingLayoutInitialized = false;
        lastSidebarVisible = currentSidebarVisible;
    }
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("DockSpace Demo", nullptr, window_flags);

    ImGui::PopStyleVar(3);

    // Customize titlebar colors to match app background
    const auto& colors = darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    ImGui::PushStyleColor(ImGuiCol_TitleBg, colors.base);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colors.base);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, colors.base);

    // Add border around dock windows using Theme colors (only when using docking)
    if (shouldUseDocking) {
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, colors.base);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
    } else {
        // During animation, hide borders to prevent flashing
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, colors.base);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }

    const ImGuiID dockSpaceId = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockSpaceId, ImVec2(0.0f, 0.0f));

    // Setup default docking layout
    setupDockingLayout(dockSpaceId);

    // Database sidebar rendering
    const bool shouldShowSidebar = sidebarWidth > 0.01f;

    if (shouldShowSidebar) {
        ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

        ImGui::SetNextWindowSizeConstraints(ImVec2(150, -1), ImVec2(500, -1));
        ImGui::Begin("Databases", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        databaseSidebar->render();
        ImGui::End();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(3);
    }

    // Main workspace area - positioning depends on sidebar visibility
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

    if (tabManager->isEmpty()) {
        // Show empty state
        const std::string workspaceTitle = getCurrentWorkspaceName();

        ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.mantle);
        ImGui::Begin(workspaceTitle.c_str(), nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        TabManager::renderEmptyState();
        ImGui::End();
        ImGui::PopStyleColor(1);
    } else {
        tabManager->renderTabs();
    }

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(3);

    // Pop titlebar colors
    ImGui::PopStyleColor(3); // TitleBg, TitleBgActive, TitleBgCollapsed

    // Pop styles and end DockSpace
    if (shouldUseDocking) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);
    } else {
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(1);
    }
    ImGui::End();
}

void Application::renderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Refresh All")) {
                for (const auto& db : databases) {
                    if (db->isConnected()) {
                        db->refreshAllTables();
                    }
                }
            }
            ImGui::EndMenu();
        }

        // Push theme toggle to the right side of the menu bar
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 100);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Dark");
        ImGui::SameLine();
        UIUtils::ToggleButton("##ThemeToggle", &darkTheme);
        if (ImGui::IsItemClicked()) {
            setDarkTheme(darkTheme);
        }

        ImGui::EndMenuBar();
    }
}

// Platform-specific methods that delegate to the platform implementation
#ifdef __APPLE__
void Application::onSidebarToggleClicked() const {
    if (platform_) {
        platform_->onSidebarToggleClicked();
    }
}

float Application::getTitlebarHeight() const {
    if (platform_) {
        return platform_->getTitlebarHeight();
    }
    return 0.0f;
}

void Application::updateWorkspaceDropdown() const {
    if (platform_) {
        platform_->updateWorkspaceDropdown();
    }
}
#endif
