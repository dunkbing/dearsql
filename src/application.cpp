#include "application.hpp"
#include "config.hpp"
#include "database/async_helper.hpp"
#include "database/cassandra.hpp"
#include "database/file_database.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "license/license_manager.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/alert.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/create_database_dialog.hpp"
#include "ui/markdown_text.hpp"
#include "ui/schema_compare.hpp"
#include "ui/settings_dialog.hpp"

#include "imshell/updater.hpp"

#if defined(DEARSQL_IOS)
#include "platform/ios_platform.hpp"
#else
#include "imshell/title_bar.hpp"
#include "ui/title_bar.hpp"
#endif

#include "themes.hpp"
#include "utils/file_dialog.hpp"
#include "utils/reaper.hpp"
#include "utils/sentry_utils.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <imgui_internal.h>
#include <iostream>
#include <limits>
#include <spdlog/spdlog.h>
#include <thread>

#include "ui/fonts.hpp"

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

    // open the pro purchase page in the user's browser
    void openPurchaseUrl() {
        const std::string url = PURCHASE_URL;
#if defined(DEARSQL_MOBILE)
        // ponytail: no purchase link on ipad yet; the stores want in-app purchases
        (void)url;
#elif defined(__APPLE__)
        std::system(("open \"" + url + "\"").c_str());
#elif defined(_WIN32)
        std::system(("start \"\" \"" + url + "\"").c_str());
#else
        std::system(("xdg-open \"" + url + "\"").c_str());
#endif
    }

    // nag unlicensed users to upgrade, at most once an hour
    void maybeRemindToUpgrade() {
        if (LicenseManager::instance().hasValidLicense())
            return;
        using clock = std::chrono::steady_clock;
        static auto nextReminder = clock::now() + std::chrono::hours(1);
        if (clock::now() < nextReminder)
            return;
        nextReminder = clock::now() + std::chrono::hours(1);
        Alert::show("Upgrade to DearSQL Pro",
                    "You're on the free version. Unlock unlimited connections and workspaces "
                    "with a one-time purchase.",
                    {{"Get Pro", [] { openPurchaseUrl(); }},
                     {"Later", nullptr, AlertButton::Style::Cancel}});
    }
} // namespace

bool Application::initialize() {
    spdlog::info("ImGui version: {}", IMGUI_VERSION);
    spdlog::info("Starting {}...", APP_NAME);

#if defined(DEARSQL_IOS)
    // imshell runs uikit; we are inside the root controller's loadView, the view exists
    if (!shell_.create(imshell::Config{}, *this))
        return false;
    ios::attach(shell_);
#else
    imshell::Config config;
#ifdef NDEBUG
    config.title = APP_NAME;
#endif
    config.width = 1600;
    config.height = 1000;
    config.titleBar = true; // ui/title_bar.cpp, beside the os window buttons
#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
    config.titleBarHeight = 36; // points (gtk: logical pixels); elsewhere the platform's own
#endif
    if (!shell_.create(config, *this))
        return false;
#ifndef __linux__
    window = shell_.glfwWindow();
#endif
#endif

    tabManager = std::make_unique<TabManager>();
    // a dropped database or schema closes its tabs before its node is freed
    retireNodeHook = [this](std::unique_ptr<IDatabaseNode> node) {
        if (tabManager)
            tabManager->retireNode(std::move(node));
    };
    databaseSidebar = std::make_unique<DatabaseSidebarNew>();
    fileDialog = std::make_unique<FileDialog>();

    appState = std::make_unique<AppState>();
    if (!appState->initialize(/*mirrorToICloud=*/true)) {
        std::cerr << "Failed to initialize app state" << std::endl;
        return false;
    }

    const std::string workspaceIdStr = appState->getSetting("current_workspace", "1");
    try {
        currentWorkspaceId = std::stoi(workspaceIdStr);
    } catch (const std::exception&) {
        currentWorkspaceId = 1;
    }

    const std::string themeStr = appState->getSetting("theme", "dark");
    imshell::SetTheme(themes::byId(themeStr));
#ifdef DEARSQL_IOS
    ios::applyTheme(isDarkTheme());
#endif

    const std::string fontScaleStr = appState->getSetting("font_scale", "1.00");
    try {
        fontScale_ = std::clamp(std::stof(fontScaleStr), 0.7f, 2.0f);
    } catch (const std::exception&) {
        fontScale_ = 1.0f;
    }
    ImGui::GetStyle().FontScaleMain = fontScale_;

    showFps_ = appState->getSetting("show_fps", "0") == "1";
    vsync_ = appState->getSetting("vsync", "1") == "1";
    try {
        maxFps_ = std::max(0, std::stoi(appState->getSetting("max_fps", "120")));
    } catch (const std::exception&) {
        maxFps_ = 120;
    }
    shell_.setVSync(vsync_);
#ifndef DEARSQL_MOBILE
    shell_.setMaxFps(maxFps_);
#endif

    LicenseManager::instance().loadStoredLicense();
    LicenseManager::instance().validateStoredLicense();

    // dearsql.dev publishes the appcast (macos, windows) and the manifest (linux)
    imshell::updater::Start({.appName = "DearSQL",
                            .version = APP_VERSION,
                            .appcastUrl = "https://dearsql.dev/appcast.xml",
                            .publicKey = SPARKLE_ED_PUBLIC_KEY,
                            .manifestUrl = "https://dearsql.dev/api/version.json",
                            // windows: the inno installer stages into install\ (DearSQL.iss)
                            .stageArguments = "/verysilent /update=true"});

    restorePreviousConnections();

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

#ifdef __APPLE__
    spdlog::info("Application initialized successfully (with Metal backend)");
#elif defined(__ANDROID__)
    spdlog::info("Application initialized successfully (with OpenGL ES backend)");
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
#if defined(DEARSQL_IOS)
    // uikit runs the loop: the view calls renderFrame() each display refresh
#else
    shell_.run();
#endif
}

void Application::cleanup() {
    spdlog::info("Cleaning up {}...", APP_NAME);
    const bool signalShutdownRequested = isShutdownRequested();
    const bool pendingAsyncWork = hasPendingAsyncWork();
    // teardown already handed off (a removed connection, a dropped database) gets a
    // bounded chance to finish; past it the fast exit leaves it to the OS
    const bool fastShutdown =
        signalShutdownRequested || pendingAsyncWork || !Reaper::drain(std::chrono::seconds(3));

    if (fastShutdown) {
        AsyncOperationControl::skipWaitOnDestroy().store(true);
    }

    // tabs hold raw pointers into the databases: they go first
    retireNodeHook = nullptr;
    tabManager.reset();

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
        // the factory deleter destroys them on the reaper
        if (!Reaper::drain(std::chrono::seconds(3))) {
            spdlog::info("Database teardown still running, exiting without it");
            AsyncOperationControl::skipWaitOnDestroy().store(true);
        } else {
            Reaper::runInline();
        }
        spdlog::debug("Databases disconnected");
    } else {
        if (signalShutdownRequested) {
            spdlog::info("Skipping database teardown during signal shutdown");
        } else if (pendingAsyncWork) {
            spdlog::info("Skipping database teardown during shutdown (async work still running)");
        } else {
            spdlog::info("Skipping database teardown during shutdown (teardown still running)");
        }
    }

    databaseSidebar.reset();
    fileDialog.reset();
    spdlog::debug("Components cleaned up");

    imshell::updater::Stop();

    MarkdownText::shutdown(); // its textures need the backend

    shell_.destroy(); // backends, imgui context, window
#if !defined(DEARSQL_MOBILE) && !defined(__linux__)
    window = nullptr;
#endif

    spdlog::debug("Application cleanup completed");
}

void Application::setTheme(const std::string_view id) {
    imshell::SetTheme(themes::byId(id));
#ifdef DEARSQL_IOS
    ios::applyTheme(isDarkTheme());
#endif
    if (appState) {
        appState->setSetting("theme", getTheme().id);
    }
    SentryUtils::addBreadcrumb("app", "Theme changed", "theme", getTheme().id);
}

void Application::setFontScale(const float scale) {
    fontScale_ = std::clamp(scale, 0.7f, 2.0f);
    ImGui::GetStyle().FontScaleMain = fontScale_;
    if (appState) {
        appState->setSetting("font_scale", std::format("{:.2f}", fontScale_));
    }
}

void Application::setShowFps(const bool on) {
    showFps_ = on;
    if (appState)
        appState->setSetting("show_fps", on ? "1" : "0");
}

void Application::setVSync(const bool on) {
    vsync_ = on;
    shell_.setVSync(on);
    if (appState)
        appState->setSetting("vsync", on ? "1" : "0");
}

void Application::setMaxFps(const int fps) {
    maxFps_ = std::max(0, fps);
#ifndef DEARSQL_MOBILE
    shell_.setMaxFps(maxFps_);
#endif
    if (appState)
        appState->setSetting("max_fps", std::to_string(maxFps_));
}

void Application::setup() {
    fonts::setup();
}

// files dragged over the window: a drop anywhere goes to openFile, so outline the whole window
static void renderFileDragHint() {
    if (!imshell::DragActive())
        return;
    const auto& c = imshell::CurrentTheme();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(vp));
    const float in = Theme::Spacing::S;
    const ImVec2 a(vp->WorkPos.x + in, vp->WorkPos.y + in);
    const ImVec2 b(vp->WorkPos.x + vp->WorkSize.x - in, vp->WorkPos.y + vp->WorkSize.y - in);
    ImVec4 tint = c.primary;
    tint.w = 0.08f;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(tint), Theme::Radius::Md);
    dl->AddRect(a, b, ImGui::GetColorU32(c.primary), Theme::Radius::Md, 0, 2.0f);

    const char* label = "Drop a SQLite, DuckDB or CSV file to open it";
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 pad(Theme::Spacing::L, Theme::Spacing::M);
    // bottom centre, clear of the empty-state text in the middle
    const float midX = (a.x + b.x) * 0.5f;
    const ImVec2 p1(midX + ts.x * 0.5f + pad.x, b.y - Theme::Spacing::XL * 2);
    const ImVec2 p0(midX - ts.x * 0.5f - pad.x, p1.y - ts.y - pad.y * 2);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(c.primary), Theme::Radius::Md);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), ImGui::GetColorU32(c.primaryForeground), label);
}

void Application::frame() {
#ifndef DEARSQL_IOS
    TitleBarView::render(*this); // in imshell's title bar strip
#endif
    renderMainUI();
    renderFileDragHint();
    Alert::render(); // last: over every other window and popup
}

bool Application::wantsFrames() {
    return AsyncOperationControl::wantsFrames();
}

bool Application::shouldQuit() {
    return isShutdownRequested();
}

bool Application::hasPendingAsyncWork() const {
    // A dump lives on the DatabaseHierarchy rather than the DatabaseInterface, so
    // the loop below cannot see it. Quitting mid-dump would then take the path that
    // disconnects, and ~ConnectionPool waits for the session the dump still holds --
    // hanging after the window has gone.
    if (databaseSidebar && databaseSidebar->hasRunningSqlDump()) {
        return true;
    }

    // tab work (a query, a table load) runs outside any DatabaseInterface
    if (AsyncOperationControl::hasRunningTasks())
        return true;

    return std::ranges::any_of(databases, [](const std::shared_ptr<DatabaseInterface>& db) {
        return db && db->hasPendingAsyncWork();
    });
}

bool Application::isShutdownRequested() const {
    return g_shutdownRequested != 0;
}

const Theme& Application::getCurrentColors() const {
    return imshell::CurrentTheme();
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

void Application::syncSavedConnections() {
    // ponytail: only the current workspace; cached ones catch up when switched back to
    const auto saved = appState->getConnectionsForWorkspace(currentWorkspaceId);
    std::vector<std::shared_ptr<DatabaseInterface>> gone;
    for (const auto& db : databases) {
        if (db && db->getConnectionId() > 0 &&
            std::ranges::none_of(
                saved, [&](const SavedConnection& s) { return s.id == db->getConnectionId(); }))
            gone.push_back(db);
    }
    for (const auto& db : gone) {
        removeDatabase(db);
    }
    for (const auto& s : saved) {
        const auto it = std::ranges::find_if(
            databases, [&](const auto& db) { return db && db->getConnectionId() == s.id; });
        if (it == databases.end()) {
            if (auto db = DatabaseFactory::createDatabase(s.connectionInfo)) {
                db->setConnectionId(s.id);
                databases.push_back(db);
            }
            continue;
        }
        // ponytail: labels only; a synced host or password change shows after a relaunch
        auto info = (*it)->getConnectionInfo();
        if (info.name != s.connectionInfo.name || info.envTag != s.connectionInfo.envTag ||
            info.color != s.connectionInfo.color) {
            info.name = s.connectionInfo.name;
            info.envTag = s.connectionInfo.envTag;
            info.color = s.connectionInfo.color;
            (*it)->setConnectionInfo(info);
        }
    }
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

    // the factory's deleter destroys it on the reaper once the last owner lets go
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

        // through the factory: its deleter runs the destructor on the reaper
        db = DatabaseFactory::createDatabase(conn.connectionInfo);
        if (!db) {
            spdlog::warn("Unknown database type {} for connection '{}', skipping",
                         static_cast<int>(conn.connectionInfo.type), conn.connectionInfo.name);
            continue;
        }

        if (db) {
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

void Application::setCurrentWorkspace(const int workspaceId) {
    spdlog::debug("setCurrentWorkspace({}) current={}", workspaceId, currentWorkspaceId);
    if (currentWorkspaceId == workspaceId) {
        spdlog::debug("setCurrentWorkspace: early return (same id)");
        return;
    }

    // the sidebar drops the hierarchies of the old workspace's connections, and a
    // dump or pg_dump/pg_restore lives on its hierarchy
    if (databaseSidebar && databaseSidebar->hasRunningSqlDump()) {
        Alert::show("Switch Workspace",
                    "Wait for the running import, export, backup or restore to finish "
                    "before switching workspaces.");
        return;
    }

    // Keep current workspace connections in memory so we only load each workspace once.
    workspaceDatabaseCache[currentWorkspaceId] = std::move(databases);

    currentWorkspaceId = workspaceId;

    if (appState) {
        appState->setSetting("current_workspace", std::to_string(currentWorkspaceId));
        appState->updateWorkspaceLastUsed(currentWorkspaceId);
    }

    SentryUtils::addBreadcrumb("app", "Workspace switched", "workspace_id",
                               std::to_string(workspaceId));

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

    return "Default";
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
    }

    return newWorkspaceId;
}

bool Application::deleteWorkspace(const int workspaceId) {
    if (!appState || workspaceId == 1) {
        return false;
    }
    // deleting the current workspace switches away, which a running dump refuses
    if (currentWorkspaceId == workspaceId && databaseSidebar &&
        databaseSidebar->hasRunningSqlDump()) {
        Alert::show("Delete Workspace",
                    "Wait for the running import, export, backup or restore to finish "
                    "before deleting this workspace.");
        return false;
    }

    bool success = appState->deleteWorkspace(workspaceId);

    if (success) {
        if (currentWorkspaceId == workspaceId) {
            setCurrentWorkspace(1);
        }
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
    return appState->renameWorkspace(workspaceId, name);
}

void Application::refreshWorkspaceConnections() {
    // tabs hold raw pointers into the databases: they go first
    if (tabManager) {
        tabManager->closeAllTabs();
    }
    databases.clear();

    if (auto cacheIt = workspaceDatabaseCache.find(currentWorkspaceId);
        cacheIt != workspaceDatabaseCache.end()) {
        databases = std::move(cacheIt->second);
        workspaceDatabaseCache.erase(cacheIt);
    } else {
        restorePreviousConnections();
    }

    clearSelectedDatabase();
}

void Application::dockTabToCenter(const std::string& tabName) const {
    // before the first frame (a file opened at launch) setupDockingLayout docks it
    if (centerDockId == 0 && !ImGui::GetCurrentContext()->CurrentWindow)
        return;
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

    if (compactLayout_) {
        // one full-width tab bar: Databases first, then the workspace or the open tabs
        ImGui::DockBuilderDockWindow("Databases", dockSpaceId);
        const std::string wsTitle = getCurrentWorkspaceName() + "###workspace_main";
        ImGui::DockBuilderDockWindow(wsTitle.c_str(), dockSpaceId);
        dockTabs(dockSpaceId);
        leftDockId = 0;
        centerDockId = dockSpaceId; // new tabs dock here too
        rightDockId = 0;
        // the connection list, not an empty workspace tab
        focusDatabases_ = !tabManager || tabManager->getTabs().empty() ? 2 : 0;
    } else if (sidebarVisible) {
        ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Left, kSidebarWidth, &leftDockId,
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
        const std::string wsTitle = getCurrentWorkspaceName() + "###workspace_main";
        ImGui::DockBuilderDockWindow(wsTitle.c_str(), dockSpaceId);
        dockTabs(dockSpaceId);

        leftDockId = 0;
        centerDockId = dockSpaceId; // GetID("MyDockSpace") outside the host window misses
        rightDockId = 0;
    }

    ImGui::DockBuilderFinish(dockSpaceId);
    dockingLayoutInitialized = true;
}

// vertical hairline at the right edge of every dock tab (TabBorderSize is all-or-nothing)
static void drawDockTabSeparators(ImGuiDockNode* node) {
    if (!node)
        return;
    if (!node->IsLeafNode()) {
        drawDockTabSeparators(node->ChildNodes[0]);
        drawDockTabSeparators(node->ChildNodes[1]);
        return;
    }
    ImGuiTabBar* bar = node->TabBar;
    if (!bar || !node->HostWindow || bar->CurrFrameVisible != ImGui::GetFrameCount())
        return;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Border);
    const ImRect& r = bar->BarRect;
    for (const ImGuiTabItem& tab : bar->Tabs) {
        const float x = r.Min.x + tab.Offset + tab.Width - bar->ScrollingAnim;
        if (x <= r.Min.x || x > r.Max.x)
            continue;
        node->HostWindow->DrawList->AddLine({x, r.Min.y}, {x, r.Max.y}, col);
    }
}

void Application::renderMainUI() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

#ifdef DEARSQL_MOBILE
    if (const bool compact = viewport->WorkSize.x < 700.0f; compact != compactLayout_) {
        compactLayout_ = compact;
        dockingLayoutInitialized = false;
    }
    if (compactLayout_) {
        sidebarVisible = true; // it is a tab, so always there
    }
#endif
    if (sidebarVisibleLastFrame_ != sidebarVisible) {
        if (tabManager) {
            tabManager->preserveFocusedTabForLayoutRebuild();
        }
        dockingLayoutInitialized = false;
        sidebarVisibleLastFrame_ = sidebarVisible;
    }
    // clear of system bars and cutouts (mobile; the ios navigation bar is in the safe area),
    // under the title bar (desktop, android)
    const imshell::Rect content = imshell::ContentRect();
    ImGui::SetNextWindowPos(content.pos);
    ImGui::SetNextWindowSize(content.size);
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
    const auto& colors = getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_TitleBg, colors.sidebar);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colors.sidebar);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, colors.sidebar);

    if (sidebarVisible) {
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, colors.background);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.mutedForeground);
    } else {
        // no sidebar, so no border between it and the tabs
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, colors.background);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }

    const ImGuiID dockSpaceId = ImGui::GetID("MyDockSpace");

    // setup layout before DockSpace so rebuilt nodes are ready this frame
    setupDockingLayout(dockSpaceId);

    // dock tab bars space tabs by ItemInnerSpacing.x; 1px gap acts as a right hairline
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
                        ImVec2(1.0f, ImGui::GetStyle().ItemInnerSpacing.y));
    ImGui::DockSpace(dockSpaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_NoCloseButton);
    ImGui::PopStyleVar();
    drawDockTabSeparators(ImGui::DockBuilderGetNode(dockSpaceId));

    if (sidebarVisible) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.background);
        ImGui::PushStyleColor(ImGuiCol_Tab, colors.background);
        ImGui::PushStyleColor(ImGuiCol_TabActive, colors.muted);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.accent);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

        if (!compactLayout_) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(150, -1), ImVec2(500, -1));
        }
        if (focusDatabases_ > 0 && focusDatabases_--)
            ImGui::SetNextWindowFocus();
        ImGui::Begin("Databases", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollbar);

        databaseSidebar->render();
        ImGui::End();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    }

    // Outside the block above on purpose. A dump outlives the sidebar being hidden,
    // and its progress panel carries the only way to cancel it; skipping the poll
    // would also leave a finished dump reporting itself as still running.
    if (databaseSidebar) {
        databaseSidebar->processDumpOperations();
    }
    if (tabManager) {
        tabManager->tickChatTabs();
    }
    SchemaCompare::poll();
    if (appState && appState->takeRemoteChange()) {
        syncSavedConnections();
    }

    ImGui::PushStyleColor(ImGuiCol_Tab, colors.background);
    ImGui::PushStyleColor(ImGuiCol_TabActive, colors.muted);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

    if (tabManager->isEmpty()) {
        const std::string workspaceTitle = getCurrentWorkspaceName() + "###workspace_main";

        ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.sidebar);
        ImGui::Begin(workspaceTitle.c_str(), nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        TabManager::renderEmptyState();
        ImGui::End();
        ImGui::PopStyleColor(1);
    } else {
        tabManager->renderTabs();
    }
    tabManager->renderOpenQuickly();

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(3);

    ImGui::PopStyleColor(3); // TitleBg, TitleBgActive, TitleBgCollapsed

    if (sidebarVisible) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);
    } else {
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(1);
    }

    ConnectionDialog::instance().render();
    CreateDatabaseDialog::instance().render();
    SettingsDialog::instance().render();
    AISettingsDialog::instance().render();

    maybeRemindToUpgrade();

    // frames per second, bottom right, above everything (not in docs screenshots)
    if (fpsCounterWidth() > 0.0f) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const std::string fps = std::format("{:.0f} FPS", ImGui::GetIO().Framerate);
        const ImVec2 size = ImGui::CalcTextSize(fps.c_str());
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - Theme::Spacing::M,
                   vp->WorkPos.y + vp->WorkSize.y - size.y - Theme::Spacing::S),
            ImGui::GetColorU32(getCurrentColors().mutedForeground), fps.c_str());
    }

    ImGui::End();
}

void Application::openFile(const std::string& rawPath) {
    if (rawPath.empty() || !tabManager || !appState)
        return;

    std::error_code ec;
    const std::string path =
        std::filesystem::weakly_canonical(std::filesystem::absolute(rawPath), ec).string();
    if (ec || path.empty())
        return;

    const std::string ext = [&] {
        auto e = std::filesystem::path(path).extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return e;
    }();

    // csv opens as a duckdb-backed connection so it shows in the sidebar and is queryable,
    // plus the csv editor; the editor reads the file itself, so a csv DuckDB cannot read
    // still opens
    const bool isCsv = ext == ".csv";
    const bool isDuckDb = (ext == ".duckdb" || ext == ".ddb" || isCsv);
    if (ext != ".db" && ext != ".sqlite" && ext != ".sqlite3" && !isDuckDb)
        return;

    for (auto& db : databases) {
        auto* fileDb = dynamic_cast<FileDatabase*>(db.get());
        if (fileDb && fileDb->getPath() == path) {
            if (!fileDb->isConnected()) {
                auto [ok, err] = fileDb->connect();
                if (!ok) {
                    spdlog::error("Failed to reconnect database file: {}", err);
                    if (!isCsv)
                        return;
                }
            }
            setSelectedDatabase(db);
            if (isCsv)
                tabManager->createCsvEditorTab(path);
            return;
        }
    }

    if (!canAddConnection()) {
        spdlog::warn("Cannot open file: connection limit reached (free tier: 3).");
        if (isCsv)
            tabManager->createCsvEditorTab(path);
        return;
    }

    DatabaseConnectionInfo info;
    info.type = isDuckDb ? DatabaseType::DUCKDB : DatabaseType::SQLITE;
    info.path = path;
    info.name = std::filesystem::path(path).filename().string();

    auto db = DatabaseFactory::createDatabase(info);
    auto [success, error] = db->connect();
    if (!success) {
        spdlog::error("Failed to open file: {}", error);
        if (!isCsv)
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
    if (isCsv)
        tabManager->createCsvEditorTab(path);
}

float Application::fpsCounterWidth() const {
    if (!showFps_ || std::getenv("DEARSQL_SHOT_DIR")) {
        return 0.0f;
    }
    return ImGui::CalcTextSize("000 FPS").x + Theme::Spacing::M * 2;
}
