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

    window = glfwCreateWindow(1280, 720, "Dear SQL", nullptr, nullptr);
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

    // Split the dockspace into left and right
    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.25f, &dock_left, &dock_right);

    // Make the left dock node non-dockable, but resizable
    // ImGui::DockBuilderGetNode(dock_left)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

    // Dock windows to specific nodes
    // Make both dock nodes non-dockable and non-tabbed
    ImGui::DockBuilderGetNode(dock_left)->LocalFlags |=
        ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;
    ImGui::DockBuilderGetNode(dock_right)->LocalFlags |=
        ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingInCentralNode;

    // Dock windows to fixed positions
    ImGui::DockBuilderDockWindow("Databases", dock_left);
    ImGui::DockBuilderDockWindow("Content", dock_right);

    ImGui::DockBuilderFinish(dockspaceId);
    dockingLayoutInitialized = true;
}

void Application::renderMainUI() {
    // DockSpace setup
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
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

    // Add border around dock windows using Theme colors
    const auto &colors = darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, colors.base);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);

    const ImGuiID dockSpaceId = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockSpaceId, ImVec2(0.0f, 0.0f));

    // Setup default docking layout
    setupDockingLayout(dockSpaceId);

    // Database sidebar with theme-based tab highlighting (fixed, non-moveable)
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);

    // Make sidebar window non-moveable and non-dockable but resizable with constraints
    ImGui::SetNextWindowSizeConstraints(ImVec2(150, -1), ImVec2(500, -1));
    ImGui::Begin("Databases", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    databaseSidebar->render();
    ImGui::End();

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(3);

    // Main content area - docked to the right, fixed in place
    ImGui::PushStyleColor(ImGuiCol_Tab, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_TabActive, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
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
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
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

float Application::getTitlebarHeight() const {
    if (platform_) {
        return platform_->getTitlebarHeight();
    }
    return 0.0f;
}
#endif
