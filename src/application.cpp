#include "application.hpp"
#include "../include/ui/tab_manager.hpp"
#include "database/postgresql.hpp"
#include "database/sqlite.hpp"
#include "platform/default_platform.hpp"
#include "platform/macos_platform.hpp"
#include "themes.hpp"
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

void Application::run() {
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

void Application::setDarkTheme(bool dark) {
    darkTheme = dark;
    Theme::ApplyNativeTheme(darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT);
}

void Application::addDatabase(const std::shared_ptr<DatabaseInterface> &db) {
    databases.push_back(db);
}

void Application::removeDatabase(size_t index) {
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

    auto savedConnections = appState->getSavedConnections();
    std::cout << "Restoring " << savedConnections.size() << " previous connections..." << std::endl;

    for (const auto &conn : savedConnections) {
        std::shared_ptr<DatabaseInterface> db = nullptr;

        if (conn.type == "postgresql") {
            db = std::make_shared<PostgresDatabase>(conn.name, conn.host, conn.port, conn.database,
                                                    conn.username, conn.password);
        } else if (conn.type == "sqlite") {
            db = std::make_shared<SQLiteDatabase>(conn.name, conn.path);
        }

        if (db) {
            std::cout << "Added connection (will connect when expanded): " << conn.name
                      << std::endl;
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

bool Application::initializeImGui() {
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
    size_t embeddedFontCount = getEmbeddedFontCount();
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

            ImFont *loadedFont = io.Fonts->AddFontFromMemoryTTF((void *)font.data, (int)font.size,
                                                                16.0f, &embeddedFontConfig, ranges);
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

void Application::setupDockingLayout(ImGuiID dockspaceId) {
    if (dockingLayoutInitialized)
        return;

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // Check if we should use docking (no animation, just visibility)
    bool shouldUseDocking = targetSidebarWidth > 0.01f;

    if (shouldUseDocking) {
        // Create split layout when stable, use current width for consistent size
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, sidebarWidth, &leftDockId,
                                    &rightDockId);

        // Make both dock nodes non-dockable and non-tabbed, but allow resizing
        ImGui::DockBuilderGetNode(leftDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;
        ImGui::DockBuilderGetNode(rightDockId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;

        // Dock windows to fixed positions
        ImGui::DockBuilderDockWindow("Databases", leftDockId);
        ImGui::DockBuilderDockWindow("Content", rightDockId);
    } else {
        // When sidebar is hidden or animating, use full space for content
        ImGui::DockBuilderGetNode(dockspaceId)->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;
        ImGui::DockBuilderDockWindow("Content", dockspaceId);
        leftDockId = 0;
        rightDockId = 0;
    }

    ImGui::DockBuilderFinish(dockspaceId);
    dockingLayoutInitialized = true;
}

void Application::renderMainUI() {
    // DockSpace setup
    const ImGuiViewport *viewport = ImGui::GetMainViewport();

    // Direct show/hide sidebar without animation
    sidebarWidth = targetSidebarWidth;

    // Always use docking when sidebar is visible for proper resizing
    bool shouldUseDocking = targetSidebarWidth > 0.01f;

    // Rebuild layout when sidebar visibility changes
    static bool lastSidebarVisible = false;
    bool currentSidebarVisible = targetSidebarWidth > 0.01f;
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

    // Menu bar removed

    ImGui::PopStyleVar(3);

    // Add border around dock windows using Theme colors (only when using docking)
    const auto &colors = darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
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
    bool shouldShowSidebar = sidebarWidth > 0.01f;

    if (shouldShowSidebar) {
        ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

        if (shouldUseDocking) {
            // Use docking system for resizing when sidebar is visible
            ImGui::SetNextWindowSizeConstraints(ImVec2(150, -1), ImVec2(500, -1));
            ImGui::Begin("Databases", nullptr,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
        } else {
            // This shouldn't happen since we only render sidebar when visible
            ImGui::Begin("Databases", nullptr,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
        }

        databaseSidebar->render();
        ImGui::End();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(3);
    }

    // Main content area - positioning depends on sidebar visibility
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

    // Always use docking system for content area
    ImGui::Begin("Content", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    if (tabManager->isEmpty()) {
        tabManager->renderEmptyState();
    } else {
        tabManager->renderTabs();
    }
    ImGui::End();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(3);

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
                for (auto &db : databases) {
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
void Application::onConnectButtonClicked() {
    if (platform_) {
        platform_->onConnectButtonClicked();
    }
}

void Application::onSidebarToggleClicked() {
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
