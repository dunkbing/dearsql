#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "imgui_impl_glfw.h"
#include "platform/default_platform.hpp"
#include "platform/macos_platform.hpp"
#include "themes.hpp"
#include "ui/log_panel.hpp"
#include "utils/file_dialog.hpp"
#include "utils/toggle_button.hpp"
#include <csignal>
#include <imgui_internal.h>
#include <iostream>

#ifdef USE_OPENGL_BACKEND
#include "imgui_impl_opengl3.h"
#endif

// Forward declarations for embedded fonts
extern "C" {
struct EmbeddedFont {
    const char *name;
    const uint8_t *data;
    size_t size;
};
const EmbeddedFont *getEmbeddedFonts();
size_t getEmbeddedFontCount();
}

static void signal_handler(int signal) {
    if (signal == SIGTERM || signal == SIGINT) {
        Application::getInstance().cleanup();
        exit(0);
    }
}

Application &Application::getInstance() {
    static Application instance;
    return instance;
}

bool Application::initialize() {
    std::cout << "Starting Dear SQL..." << std::endl;

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
    databaseSidebar = std::make_unique<DatabaseSidebar>();
    fileDialog = std::make_unique<FileDialog>();

    // Initialize app state
    appState = std::make_unique<AppState>();
    if (!appState->initialize()) {
        std::cerr << "Failed to initialize app state" << std::endl;
        return false;
    }

    // Restore previous connections
    restorePreviousConnections();

    // Register signal handler
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Setup title bar after window creation
    platform_->setupTitlebar();

#ifdef USE_METAL_BACKEND
    std::cout << "Application initialized successfully (with Metal backend)" << std::endl;
#else
    std::cout << "Application initialized successfully (with OpenGL backend)" << std::endl;
#endif
    return true;
}

void Application::run() const {
#ifdef USE_OPENGL_BACKEND
    glClearColor(darkTheme ? 0.110f : 0.957f, darkTheme ? 0.110f : 0.957f,
                 darkTheme ? 0.137f : 0.957f, 0.98f);
#endif

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

#ifdef USE_METAL_BACKEND
        platform_->renderFrame();
#elif defined(USE_OPENGL_BACKEND)
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
    }
}

void Application::cleanup() {
    std::cout << "Cleaning up Dear SQL..." << std::endl;

    // Cleanup databases
    for (auto &db : databases) {
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

void Application::addDatabase(const std::shared_ptr<DatabaseInterface> &db) {
    databases.push_back(db);
}

void Application::removeDatabase(const int index) {
    if (index < databases.size()) {
        const auto &db = databases[index];
        if (db) {
            db->disconnect();
        }
        databases.erase(databases.begin() + index);

        // Update selection if needed
        if (selectedDatabase == static_cast<int>(index)) {
            selectedDatabase = -1;
            selectedTable = -1;
        } else if (selectedDatabase > static_cast<int>(index)) {
            selectedDatabase--;
        }
    }
}

void Application::restorePreviousConnections() {
    if (!appState) {
        return;
    }

    const auto savedConnections = appState->getSavedConnections();
    LogPanel::info("Restoring " + std::to_string(savedConnections.size()) +
                   " previous connections");

    for (const auto &conn : savedConnections) {
        std::shared_ptr<DatabaseInterface> db = nullptr;

        if (conn.type == "postgresql") {
            db = std::make_shared<PostgresDatabase>(conn.name, conn.host, conn.port, conn.database,
                                                    conn.username, conn.password, true);
        } else if (conn.type == "mysql") {
            db = std::make_shared<MySQLDatabase>(conn.name, conn.host, conn.port, conn.database,
                                                 conn.username, conn.password);
        } else if (conn.type == "sqlite") {
            db = std::make_shared<SQLiteDatabase>(conn.name, conn.path);
        } else if (conn.type == "redis") {
            db = std::make_shared<RedisDatabase>(conn.name, conn.host, conn.port, conn.password,
                                                 conn.username);
        }

        if (db) {
            LogPanel::debug("Added connection (will connect when expanded): " + conn.name);
            databases.push_back(db);
        }
    }
}

bool Application::initializeGLFW() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }

#ifdef USE_METAL_BACKEND
    // Metal backend doesn't need OpenGL context hints
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#elif defined(USE_OPENGL_BACKEND)
    // OpenGL backend requires context hints
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

#ifdef NDEBUG
    const auto title = "Dear SQL";
#else
    const auto title = "";
#endif
    window = glfwCreateWindow(1280, 720, title, nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

#ifdef USE_OPENGL_BACKEND
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
#endif

    std::cout << "GLFW window created successfully" << std::endl;
    return true;
}

bool Application::initializeImGui() const {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    setupFonts();

    ImGui::StyleColorsDark();
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);

    // Initialize GLFW backend
#ifdef USE_METAL_BACKEND
    ImGui_ImplGlfw_InitForOther(window, true);
#elif defined(USE_OPENGL_BACKEND)
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#endif

    // Initialize platform-specific ImGui backend
    if (!platform_->initializeImGuiBackend()) {
        std::cerr << "Failed to initialize ImGui backend" << std::endl;
        return false;
    }

#ifdef USE_METAL_BACKEND
    std::cout << "ImGui initialized with Metal backend" << std::endl;
#elif defined(USE_OPENGL_BACKEND)
    std::cout << "ImGui initialized with OpenGL backend" << std::endl;
#endif

    return true;
}

void Application::setupFonts() {
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig fontConfig;

    // load embedded fonts first
    const size_t embeddedFontCount = getEmbeddedFontCount();
    if (embeddedFontCount > 0) {
        const EmbeddedFont *embeddedFonts = getEmbeddedFonts();
        for (size_t i = 0; i < embeddedFontCount; ++i) {
            const EmbeddedFont &font = embeddedFonts[i];

            // Choose appropriate glyph ranges based on font name
            const ImWchar *ranges = nullptr;
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

            const ImFont *loadedFont = io.Fonts->AddFontFromMemoryTTF(
                (void *)font.data, static_cast<int>(font.size), 16.0f, &embeddedFontConfig, ranges);
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
#ifdef USE_OPENGL_BACKEND
    io.Fonts->Build();
#endif
}

void Application::setupDockingLayout(const ImGuiID dockSpaceId) {
    if (dockingLayoutInitialized)
        return;

    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, ImGui::GetMainViewport()->Size);

    // Check if we should use docking for sidebar and log panel
    bool shouldUseSidebar = targetSidebarWidth > 0.01f;
    bool shouldUseLogPanel = targetLogPanelWidth > 0.01f;

    if (shouldUseSidebar && shouldUseLogPanel) {
        // Three-panel layout: sidebar | center | log panel
        ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Left, sidebarWidth, &leftDockId,
                                    &centerDockId);
        ImGui::DockBuilderSplitNode(centerDockId, ImGuiDir_Right,
                                    logPanelWidth / (1.0f - sidebarWidth), &rightDockId,
                                    &centerDockId);

        // Configure dock nodes
        ImGui::DockBuilderGetNode(leftDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;
        ImGui::DockBuilderGetNode(rightDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;

        // Dock windows
        ImGui::DockBuilderDockWindow("Databases", leftDockId);
        ImGui::DockBuilderDockWindow("Logs", rightDockId);
        ImGui::DockBuilderDockWindow("Workspace", centerDockId);

        for (const auto &tab : tabManager->getTabs()) {
            ImGui::DockBuilderDockWindow(tab->getName().c_str(), centerDockId);
        }
    } else if (shouldUseSidebar) {
        // Two-panel layout: sidebar | center
        ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Left, sidebarWidth, &leftDockId,
                                    &centerDockId);

        ImGui::DockBuilderGetNode(leftDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;

        ImGui::DockBuilderDockWindow("Databases", leftDockId);
        ImGui::DockBuilderDockWindow("Workspace", centerDockId);

        for (const auto &tab : tabManager->getTabs()) {
            ImGui::DockBuilderDockWindow(tab->getName().c_str(), centerDockId);
        }

        rightDockId = 0;
    } else if (shouldUseLogPanel) {
        // Two-panel layout: center | log panel
        ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Right, logPanelWidth, &rightDockId,
                                    &centerDockId);

        ImGui::DockBuilderGetNode(rightDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;

        ImGui::DockBuilderDockWindow("Logs", rightDockId);
        ImGui::DockBuilderDockWindow("Workspace", centerDockId);

        for (const auto &tab : tabManager->getTabs()) {
            ImGui::DockBuilderDockWindow(tab->getName().c_str(), centerDockId);
        }

        leftDockId = 0;
    } else {
        // Single panel layout: just center
        ImGui::DockBuilderDockWindow("Workspace", dockSpaceId);

        for (const auto &tab : tabManager->getTabs()) {
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
    const ImGuiViewport *viewport = ImGui::GetMainViewport();

    // Direct show/hide sidebar and log panel without animation
    sidebarWidth = targetSidebarWidth;
    logPanelWidth = targetLogPanelWidth;

    // Always use docking when sidebar or log panel is visible for proper resizing
    const bool shouldUseDocking = targetSidebarWidth > 0.01f || targetLogPanelWidth > 0.01f;

    // Rebuild layout when sidebar or log panel visibility changes
    static bool lastSidebarVisible = false;
    static bool lastLogPanelVisible = false;
    const bool currentSidebarVisible = targetSidebarWidth > 0.01f;
    const bool currentLogPanelVisible = targetLogPanelWidth > 0.01f;
    if (lastSidebarVisible != currentSidebarVisible ||
        lastLogPanelVisible != currentLogPanelVisible) {
        dockingLayoutInitialized = false;
        lastSidebarVisible = currentSidebarVisible;
        lastLogPanelVisible = currentLogPanelVisible;
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
    const auto &colors = darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
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
    const bool shouldShowLogPanel = logPanelWidth > 0.01f;

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

    // Log panel rendering
    if (shouldShowLogPanel) {
        ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

        ImGui::SetNextWindowSizeConstraints(ImVec2(200, -1), ImVec2(600, -1));
        ImGui::Begin("Logs", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

        LogPanel::getInstance().render();
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
        // Show empty state in a dockable Workspace window when no tabs are open
        ImGui::Begin("Workspace", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        tabManager->renderEmptyState();
        ImGui::End();
    } else {
        // Render individual dockable tab windows
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
                for (const auto &db : databases) {
                    if (db->isConnected()) {
                        db->refreshTables();
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
#ifdef USE_METAL_BACKEND
void Application::onSidebarToggleClicked() const {
    if (platform_) {
        platform_->onSidebarToggleClicked();
    }
}

void Application::onLogPanelToggleClicked() const {
    if (platform_) {
        platform_->onLogPanelToggleClicked();
    }
}

float Application::getTitlebarHeight() const {
    if (platform_) {
        return platform_->getTitlebarHeight();
    }
    return 0.0f;
}
#endif
