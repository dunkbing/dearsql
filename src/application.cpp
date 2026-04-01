#include "application.hpp"
#include "config.hpp"
#include "database/async_helper.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "license/license_manager.hpp"

#include "platform/updater.hpp"

#if defined(__APPLE__)
#include "platform/macos_platform.hpp"
#elif defined(__linux__)
#include "platform/linux_platform.hpp"
#elif defined(_WIN32)
#include "platform/windows_platform.hpp"
#endif

#include "themes.hpp"
#include "utils/file_dialog.hpp"
#include "utils/sentry_utils.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <imgui_internal.h>
#include <limits>
#include <spdlog/spdlog.h>

#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "embedded_fonts.hpp"

namespace {
    volatile sig_atomic_t g_shutdownRequested = 0;

    void signal_handler(const int signal) {
        if (signal == SIGTERM || signal == SIGINT) {
            if (g_shutdownRequested != 0) {
                _Exit(130);
            }
            g_shutdownRequested = 1;
        }
    }
} // namespace

Application& Application::getInstance() {
    static Application instance;
    return instance;
}

namespace {
    constexpr double kIdleActivationDelaySeconds = 2.0; // time after last activity before idling
    constexpr double kMinimumWaitSeconds = 1.0 / 120.0; // keep responsive when active
    constexpr double kMaximumWaitSeconds = 0.2;         // cap sleep to keep UI responsive

    constexpr std::size_t kFreeConnectionLimit = 3;
    constexpr std::size_t kFreeWorkspaceLimit = 1;

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
    spdlog::info("ImGui version: {}", IMGUI_VERSION);
    spdlog::info("Starting {}...", APP_NAME);

    // Initialize platform-specific components
#ifdef __APPLE__
    platform_ = std::make_unique<MacOSPlatform>(this);
    if (!initializeGLFW()) {
        return false;
    }
    if (!platform_->initializePlatform(window)) {
        spdlog::error("Failed to initialize platform");
        return false;
    }
    if (!initializeImGui()) {
        return false;
    }
#elif defined(__linux__)
    platform_ = std::make_unique<LinuxPlatform>(this);
    auto* linuxPlatform = static_cast<LinuxPlatform*>(platform_.get());
    if (!linuxPlatform->initializeGTK(nullptr, nullptr)) {
        return false;
    }
    setupImGuiContext();
    // Setup titlebar before showing window
    platform_->setupTitlebar();
#elif defined(_WIN32)
    platform_ = std::make_unique<WindowsPlatform>(this);
    if (!initializeGLFW()) {
        return false;
    }
    if (!platform_->initializePlatform(window)) {
        spdlog::error("Failed to initialize platform");
        return false;
    }
    if (!initializeImGui()) {
        return false;
    }
#endif

    // Initialize NFD
    if (!FileDialog::initialize()) {
        spdlog::error("Failed to initialize Native File Dialog");
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
    try {
        currentWorkspaceId = std::stoi(workspaceIdStr);
    } catch (const std::exception&) {
        currentWorkspaceId = 1;
    }

    // Restore theme from settings
    const std::string themeStr = appState->getSetting("theme", "dark");
    darkTheme = (themeStr != "light");
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);
#if defined(__linux__)
    static_cast<LinuxPlatform*>(platform_.get())->applyCurrentTheme();
#endif

    // Restore font scale from settings
    const std::string fontScaleStr = appState->getSetting("font_scale", "1.00");
    try {
        fontScale_ = std::clamp(std::stof(fontScaleStr), 0.7f, 2.0f);
    } catch (const std::exception&) {
        fontScale_ = 1.0f;
    }
    ImGui::GetStyle().FontScaleMain = fontScale_;

    // Load stored license and revalidate against server in background
    LicenseManager::instance().loadStoredLicense();
    LicenseManager::instance().validateStoredLicense();

    initializeUpdater();

    restorePreviousConnections();

    // Register signal handler
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

#ifndef __linux__
    // Setup title bar after window creation (Linux does it earlier)
    platform_->setupTitlebar();
#endif

    platform_->updateWorkspaceDropdown();

#ifdef __APPLE__
    spdlog::info("Application initialized successfully (with Metal backend)");
#elif defined(__linux__)
    spdlog::info("Application initialized successfully (with GTK4 + OpenGL backend)");
#elif defined(_WIN32)
    spdlog::info("Application initialized successfully (with DirectX 11 backend)");
#else
    spdlog::info("Application initialized successfully (with OpenGL backend)");
#endif

    SentryUtils::addBreadcrumb("app", "Application initialized");
    return true;
}

void Application::run() {
#if defined(__linux__)
    // Linux uses GTK main loop
    auto* linuxPlatform = static_cast<LinuxPlatform*>(platform_.get());
    linuxPlatform->runMainLoop();
#else
    // macOS and Windows use GLFW main loop
    // D3D11 handles clear color in WindowsPlatform::renderFrame()

    double lastInteractionTime = glfwGetTime();
    bool lastWindowFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
    while (!glfwWindowShouldClose(window)) {
        if (isShutdownRequested()) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        const double frameStart = glfwGetTime();
        const double timeSinceInteraction = frameStart - lastInteractionTime;
        const bool windowFocusedAtFrameStart = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
        const bool hadAsyncWork = AsyncOperationControl::hasRunningTasks();

        double waitTimeout = 0.0;
        const bool idleBecauseUnfocused = !windowFocusedAtFrameStart && !hadAsyncWork;
        const bool idleBecauseInactive = windowFocusedAtFrameStart &&
                                         (timeSinceInteraction >= kIdleActivationDelaySeconds) &&
                                         !hadAsyncWork;
        if (idleBecauseUnfocused) {
            waitTimeout = kMaximumWaitSeconds;
        } else if (idleBecauseInactive) {
            const double idleTime =
                std::max(0.0, timeSinceInteraction - kIdleActivationDelaySeconds);
            waitTimeout = std::clamp(idleTime, kMinimumWaitSeconds, kMaximumWaitSeconds);
        }

        if (waitTimeout > 0.0) {
            glfwWaitEventsTimeout(waitTimeout);
        } else {
            glfwPollEvents();
        }

        const bool windowFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
        if (windowFocused && !lastWindowFocused) {
            lastInteractionTime = glfwGetTime();
        }
        lastWindowFocused = windowFocused;

        const bool hasAsyncWork = AsyncOperationControl::hasRunningTasks();

        // never call renderFrame() when unfocused: CAMetalLayer::nextDrawable blocks
        // when the window is backgrounded, causing sluggish app switches
        if (!windowFocused) {
            continue;
        }

        platform_->renderFrame();

        const bool userActive = isImGuiUserActive();

        if (userActive || hasAsyncWork) {
            lastInteractionTime = glfwGetTime();
        }
    }
#endif
}

void Application::cleanup() {
    spdlog::info("Cleaning up {}...", APP_NAME);
    const bool signalShutdownRequested = isShutdownRequested();
    const bool pendingAsyncWork = hasPendingAsyncWork();
    const bool fastShutdown = signalShutdownRequested || pendingAsyncWork;

    if (fastShutdown) {
        AsyncOperationControl::skipWaitOnDestroy().store(true);
    }

    // Cleanup databases
    if (!fastShutdown) {
        for (auto& db : databases) {
            if (db) {
                db->disconnect();
            }
        }
        databases.clear();

        for (auto& cacheEntry : workspaceDatabaseCache) {
            auto& cachedDatabases = cacheEntry.second;
            for (auto& db : cachedDatabases) {
                if (db) {
                    db->disconnect();
                }
            }
        }
        workspaceDatabaseCache.clear();
        spdlog::debug("Databases disconnected");
    } else {
        if (signalShutdownRequested) {
            spdlog::info("Skipping database teardown during signal shutdown");
        } else if (pendingAsyncWork) {
            spdlog::info("Skipping database teardown during shutdown (async work still running)");
        }
    }

    // Cleanup components
    tabManager.reset();
    databaseSidebar.reset();
    fileDialog.reset();
    spdlog::debug("Components cleaned up");

    // Cleanup NFD
    FileDialog::cleanup();
    spdlog::debug("File dialog cleaned up");

    cleanupUpdater();

    if (platform_) {
        platform_->shutdownImGui();
        platform_->cleanup();
        platform_.reset();
    }

    ImGui::DestroyContext();
    spdlog::debug("ImGui context destroyed");

#if defined(__APPLE__) || defined(_WIN32)
    if (window) {
        glfwDestroyWindow(window);
    }
    glfwTerminate();
#endif

    spdlog::debug("Application cleanup completed");
}

void Application::setDarkTheme(const bool dark) {
    darkTheme = dark;
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);
    if (appState) {
        appState->setSetting("theme", darkTheme ? "dark" : "light");
    }
    SentryUtils::addBreadcrumb("app", "Theme changed", "theme", darkTheme ? "dark" : "light");
}

void Application::setFontScale(const float scale) {
    fontScale_ = std::clamp(scale, 0.7f, 2.0f);
    ImGui::GetStyle().FontScaleMain = fontScale_;
    if (appState) {
        appState->setSetting("font_scale", std::format("{:.2f}", fontScale_));
    }
}

bool Application::hasPendingAsyncWork() const {
    return std::ranges::any_of(databases, [](const std::shared_ptr<DatabaseInterface>& db) {
        return db && db->hasPendingAsyncWork();
    });
}

bool Application::isShutdownRequested() const {
    return g_shutdownRequested != 0;
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

bool Application::canAddConnection() const {
    if (LicenseManager::instance().hasValidLicense())
        return true;
    return databases.size() < kFreeConnectionLimit;
}

bool Application::canAddWorkspace() const {
    if (LicenseManager::instance().hasValidLicense())
        return true;
    return appState->getWorkspaces().size() < kFreeWorkspaceLimit;
}

int Application::saveConnection(const SavedConnection& conn) {
    if (!canAddConnection()) {
        spdlog::warn("Connection limit reached (free tier: {}). Upgrade to add more.",
                     kFreeConnectionLimit);
        return -2;
    }
    return appState->saveConnection(conn);
}

void Application::removeDatabase(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        return;
    }

    const auto it = std::ranges::find(databases, db);
    if (it == databases.end()) {
        return;
    }

    if (tabManager)
        tabManager->closeTabsForDatabase(db.get());

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

    const auto restoreStart = std::chrono::steady_clock::now();
    const auto savedConnections = appState->getConnectionsForWorkspace(currentWorkspaceId);
    spdlog::debug("Restoring {} connections for current workspace", savedConnections.size());
    std::size_t restoredCount = 0;

    for (const auto& conn : savedConnections) {
        std::shared_ptr<DatabaseInterface> db = nullptr;

        if (conn.connectionInfo.type == DatabaseType::POSTGRESQL ||
            conn.connectionInfo.type == DatabaseType::REDSHIFT) {
            db = std::make_shared<PostgresDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::MYSQL ||
                   conn.connectionInfo.type == DatabaseType::MARIADB) {
            db = std::make_shared<MySQLDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::SQLITE) {
            db = std::make_shared<SQLiteDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::REDIS) {
            db = std::make_shared<RedisDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::MONGODB) {
            db = std::make_shared<MongoDBDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::MSSQL) {
            db = std::make_shared<MSSQLDatabase>(conn.connectionInfo);
        } else if (conn.connectionInfo.type == DatabaseType::ORACLE) {
            db = std::make_shared<OracleDatabase>(conn.connectionInfo);
        } else {
            spdlog::warn("Unknown database type {} for connection '{}', skipping",
                         static_cast<int>(conn.connectionInfo.type), conn.connectionInfo.name);
            continue;
        }

        if (db) {
            // Store the saved connection ID in the database instance
            db->setConnectionId(conn.id);
            spdlog::debug("Added connection: {} {}", conn.connectionInfo.name,
                          conn.connectionInfo.database);
            databases.push_back(db);
            ++restoredCount;
        }
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - restoreStart)
                               .count();
    spdlog::debug("Restored {} connections for workspace {} in {} ms", restoredCount,
                  currentWorkspaceId, elapsedMs);
}

#if defined(__APPLE__) || defined(_WIN32)
bool Application::initializeGLFW() {
    if (!glfwInit()) {
        spdlog::error("Failed to initialize GLFW");
        return false;
    }

#if defined(__APPLE__) || defined(_WIN32)
    // Metal and D3D11 backend doesn't need OpenGL context hints
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif

#ifdef NDEBUG
    const auto title = APP_NAME;
#else
    const auto title = "";
#endif
    window = glfwCreateWindow(1280, 720, title, nullptr, nullptr);
    if (!window) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    spdlog::info("GLFW window created successfully");
    return true;
}
#endif

void Application::setupImGuiContext() const {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    setupFonts();
    ImGui::StyleColorsDark();
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);
}

#if defined(__APPLE__) || defined(_WIN32)
bool Application::initializeImGui() const {
    setupImGuiContext();

    if (!platform_->initializeImGuiBackend()) {
        spdlog::error("Failed to initialize ImGui backend");
        return false;
    }

#ifdef __APPLE__
    spdlog::info("ImGui initialized with Metal backend");
#elif defined(_WIN32)
    spdlog::info("ImGui initialized with DirectX 11 backend");
#endif

    return true;
}
#endif

void Application::setupFonts() {
    const ImGuiIO& io = ImGui::GetIO();

    const size_t embeddedFontCount = getEmbeddedFontCount();
    if (embeddedFontCount == 0)
        return;

    const EmbeddedFont* embeddedFonts = getEmbeddedFonts();
    ImFontConfig fontConfig;

    // icon glyph ranges (only load codepoints we actually need)
    static const ImWchar faRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    static const ImWchar fkRanges[] = {ICON_MIN_FK, ICON_MAX_16_FK, 0};

    for (size_t i = 0; i < embeddedFontCount; ++i) {
        const EmbeddedFont& font = embeddedFonts[i];
        std::string fontName = font.name;

        const ImWchar* ranges = nullptr;
        bool isIconFont = false;

        if (fontName.find("fa-solid") != std::string::npos ||
            fontName.find("fa-regular") != std::string::npos) {
            ranges = faRanges;
            isIconFont = true;
        } else if (fontName.find("forkawesome") != std::string::npos) {
            ranges = fkRanges;
            isIconFont = true;
        } else if (fontName.find("Cyrillic") != std::string::npos) {
            ranges = io.Fonts->GetGlyphRangesCyrillic();
        } else {
            ranges = io.Fonts->GetGlyphRangesDefault();
        }

        ImFontConfig embeddedFontConfig = fontConfig;
        embeddedFontConfig.FontDataOwnedByAtlas = false;

        // icon fonts don't need oversampling
        if (isIconFont) {
            embeddedFontConfig.OversampleH = 1;
            embeddedFontConfig.OversampleV = 1;
            embeddedFontConfig.PixelSnapH = true;
        }

        const ImFont* loadedFont = io.Fonts->AddFontFromMemoryTTF(
            (void*)font.data, static_cast<int>(font.size), 16.0f, &embeddedFontConfig, ranges);
        if (!fontConfig.MergeMode) {
            fontConfig.MergeMode = true;
        }

        if (loadedFont) {
            spdlog::debug("loaded embedded font: {}", fontName);
        }
    }
}

void Application::setCurrentWorkspace(const int workspaceId) {
    spdlog::debug("setCurrentWorkspace({}) current={}", workspaceId, currentWorkspaceId);
    if (currentWorkspaceId == workspaceId) {
        spdlog::debug("setCurrentWorkspace: early return (same id)");
        return;
    }

    // Keep current workspace connections in memory so we only load each workspace once.
    workspaceDatabaseCache[currentWorkspaceId] = std::move(databases);

    currentWorkspaceId = workspaceId;

    // Save current workspace to settings
    if (appState) {
        appState->setSetting("current_workspace", std::to_string(currentWorkspaceId));
        appState->updateWorkspaceLastUsed(currentWorkspaceId);
    }

    SentryUtils::addBreadcrumb("app", "Workspace switched", "workspace_id",
                               std::to_string(workspaceId));

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

    if (!canAddWorkspace()) {
        spdlog::warn("Workspace limit reached (free tier: {}). Upgrade to create more.",
                     kFreeWorkspaceLimit);
        return -2;
    }

    Workspace workspace;
    workspace.name = name;
    workspace.description = description;

    const int newWorkspaceId = appState->saveWorkspace(workspace);
    spdlog::debug("createWorkspace: saved with id={}", newWorkspaceId);

    if (newWorkspaceId > 0) {
        setCurrentWorkspace(newWorkspaceId);
        spdlog::debug("createWorkspace: after setCurrentWorkspace, currentId={}",
                      currentWorkspaceId);
        platform_->updateWorkspaceDropdown();
        spdlog::debug("createWorkspace: after updateWorkspaceDropdown, currentId={}",
                      currentWorkspaceId);
    }

    return newWorkspaceId;
}

bool Application::deleteWorkspace(const int workspaceId) {
    if (!appState || workspaceId == 1) { // Can't delete default workspace
        return false;
    }

    bool success = appState->deleteWorkspace(workspaceId);

    if (success) {
        // If we deleted the current workspace, switch to default
        if (currentWorkspaceId == workspaceId) {
            setCurrentWorkspace(1);
        }
        // disconnect cached connections before dropping them
        if (auto cacheIt = workspaceDatabaseCache.find(workspaceId);
            cacheIt != workspaceDatabaseCache.end()) {
            for (auto& db : cacheIt->second) {
                if (db) {
                    db->disconnect();
                }
            }
            workspaceDatabaseCache.erase(cacheIt);
        }
    }

    return success;
}

bool Application::renameWorkspace(const int workspaceId, const std::string& name) {
    if (!appState || name.empty())
        return false;
    bool success = appState->renameWorkspace(workspaceId, name);
    if (success)
        platform_->updateWorkspaceDropdown();
    return success;
}

void Application::refreshWorkspaceConnections() {
    databases.clear();

    // close tabs that reference nodes from the previous workspace
    if (tabManager) {
        tabManager->closeAllTabs();
    }

    // First switch to cached connections if this workspace was loaded before.
    if (auto cacheIt = workspaceDatabaseCache.find(currentWorkspaceId);
        cacheIt != workspaceDatabaseCache.end()) {
        databases = std::move(cacheIt->second);
        workspaceDatabaseCache.erase(cacheIt);
    } else {
        // First time visiting this workspace in this session: load from app state.
        restorePreviousConnections();
    }

    clearSelectedDatabase();
}

void Application::dockTabToCenter(const std::string& tabName) const {
    const ImGuiID dockId = centerDockId != 0 ? centerDockId : ImGui::GetID("MyDockSpace");
    ImGui::DockBuilderDockWindow(tabName.c_str(), dockId);
}

void Application::setupDockingLayout(const ImGuiID dockSpaceId) {
    if (dockingLayoutInitialized)
        return;

    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, ImGui::GetWindowSize());

    const std::string preferredTabWindowName =
        tabManager ? tabManager->getPreferredTabWindowNameForDocking() : std::string{};
    auto dockTabs = [&](const ImGuiID targetDockId) {
        if (!tabManager) {
            return;
        }

        for (const auto& tab : tabManager->getTabs()) {
            if (tab && tab->getWindowName() != preferredTabWindowName) {
                ImGui::DockBuilderDockWindow(tab->getWindowName().c_str(), targetDockId);
            }
        }

        if (!preferredTabWindowName.empty()) {
            ImGui::DockBuilderDockWindow(preferredTabWindowName.c_str(), targetDockId);
        }
    };

    // Check if we should use docking for sidebar
    const bool shouldUseSidebar = targetSidebarWidth > 0.01f;

    if (shouldUseSidebar) {
        // Two-panel layout: sidebar | center
        ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Left, sidebarWidth, &leftDockId,
                                    &centerDockId);

        if (auto* leftNode = ImGui::DockBuilderGetNode(leftDockId)) {
            const auto sidebarFlags = static_cast<ImGuiDockNodeFlags>(
                ImGuiDockNodeFlags_NoTabBar |
                static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDockingInCentralNode));
            leftNode->SetLocalFlags(leftNode->LocalFlags | sidebarFlags);
        }

        ImGui::DockBuilderDockWindow("Databases", leftDockId);
        const std::string wsTitle = getCurrentWorkspaceName() + "###workspace_main";
        ImGui::DockBuilderDockWindow(wsTitle.c_str(), centerDockId);
        dockTabs(centerDockId);

        rightDockId = 0;
    } else {
        // Single panel layout: just center
        const std::string wsTitle = getCurrentWorkspaceName() + "###workspace_main";
        ImGui::DockBuilderDockWindow(wsTitle.c_str(), dockSpaceId);
        dockTabs(dockSpaceId);

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
    const bool currentSidebarVisible = targetSidebarWidth > 0.01f;
    if (lastSidebarVisible_ != currentSidebarVisible) {
        if (tabManager) {
            tabManager->preserveFocusedTabForLayoutRebuild();
        }
        dockingLayoutInitialized = false;
        lastSidebarVisible_ = currentSidebarVisible;
    }
    const float topInset = platform_ ? platform_->getClientAreaTopInset() : 0.0f;
    const ImVec2 dockPos(viewport->Pos.x, viewport->Pos.y + topInset);
    const ImVec2 dockSize(viewport->Size.x, std::max(0.0f, viewport->Size.y - topInset));
    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
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

    // Customize titlebar colors — use mantle so the dock tab bar visually
    // matches the native titlebar through the vibrancy layer.
    const auto& colors = darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    ImGui::PushStyleColor(ImGuiCol_TitleBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, colors.mantle);

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

    // setup layout before DockSpace so rebuilt nodes are ready this frame
    setupDockingLayout(dockSpaceId);

    ImGui::DockSpace(dockSpaceId, ImVec2(0.0f, 0.0f));

    // Database sidebar rendering
    const bool shouldShowSidebar = sidebarWidth > 0.01f;

    if (shouldShowSidebar) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.base);
        ImGui::PushStyleColor(ImGuiCol_Tab, colors.base);
        ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

        ImGui::SetNextWindowSizeConstraints(ImVec2(150, -1), ImVec2(500, -1));
        ImGui::Begin("Databases", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollbar);

        databaseSidebar->render();
        ImGui::End();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    }

    // Main workspace area - positioning depends on sidebar visibility
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.base);
    ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

    if (tabManager->isEmpty()) {
        // Show empty state — stable ###ID so the window keeps its dock position across switches
        const std::string workspaceTitle = getCurrentWorkspaceName() + "###workspace_main";

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

    pollUpdater();

    ImGui::End();
}

void Application::openFile(const std::string& rawPath) {
    if (rawPath.empty() || !tabManager || !appState)
        return;

    // normalize to absolute path so duplicates and relative paths are detected correctly
    std::error_code ec;
    const std::string path =
        std::filesystem::weakly_canonical(std::filesystem::absolute(rawPath), ec).string();
    if (ec || path.empty())
        return;

    // case-insensitive extension check
    const std::string ext = [&] {
        auto e = std::filesystem::path(path).extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return e;
    }();

    if (ext == ".csv") {
        tabManager->createCsvEditorTab(path);
        return;
    }

    if (ext != ".db" && ext != ".sqlite" && ext != ".sqlite3")
        return;

    // reuse existing connection for this path if already open and connected
    for (auto& db : databases) {
        auto* sqliteDb = dynamic_cast<SQLiteDatabase*>(db.get());
        if (sqliteDb && sqliteDb->getPath() == path) {
            if (!sqliteDb->isConnected()) {
                auto [ok, err] = sqliteDb->connect();
                if (!ok) {
                    spdlog::error("Failed to reconnect SQLite file: {}", err);
                    return;
                }
            }
            setSelectedDatabase(db);
            return;
        }
    }

    if (!canAddConnection()) {
        spdlog::warn("Cannot open file: connection limit reached (free tier: 3).");
        return;
    }

    DatabaseConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.path = path;
    info.name = std::filesystem::path(path).filename().string();

    auto db = std::make_shared<SQLiteDatabase>(info);
    auto [success, error] = db->connect();
    if (!success) {
        spdlog::error("Failed to open file: {}", error);
        return;
    }

    SavedConnection conn;
    conn.connectionInfo = info;
    conn.workspaceId = currentWorkspaceId;
    int newId = saveConnection(conn);
    if (newId > 0)
        db->setConnectionId(newId);

    addDatabase(db);
    setSelectedDatabase(db);
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
#endif
