#include "application.hpp"
#include "database/db.hpp"
#include "database/postgresql.hpp"
#include "database/sqlite.hpp"
#include "imgui_impl_metal.h"
#include "tabs/tab_manager.hpp"
#include "themes.hpp"
#include "utils/file_dialog.hpp"
#include "utils/toggle_button.hpp"
#include <fstream>
#include <imgui_internal.h>
#include <iostream>
#include <csignal>

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

#ifdef USE_METAL_BACKEND
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

// Toolbar delegate interface
@interface ToolbarDelegate : NSObject <NSToolbarDelegate>
@property(nonatomic, assign) Application *app;
@end

@implementation ToolbarDelegate
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar {
    return @[ @"ConnectButton", NSToolbarFlexibleSpaceItemIdentifier ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    return @[ @"ConnectButton", NSToolbarFlexibleSpaceItemIdentifier ];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)itemIdentifier
    willBeInsertedIntoToolbar:(BOOL)flag {
    if ([itemIdentifier isEqualToString:@"ConnectButton"]) {
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:itemIdentifier];
        item.label = @"";
        item.paletteLabel = @"Connect";
        item.toolTip = @"Connect to Database";

        NSButton *button = [[NSButton alloc] init];
        [button setTitle:@"Connect"];
        [button setButtonType:NSButtonTypeMomentaryPushIn];
        [button setBezelStyle:NSBezelStyleRounded];
        [button setTarget:self];
        [button setAction:@selector(connectButtonClicked:)];
        [button sizeToFit];

        item.view = button;
        return item;
    }
    return nil;
}

- (void)connectButtonClicked:(id)sender {
    @try {
        if (self.app) {
            self.app->onConnectButtonClicked();
        }
    } @catch (NSException *exception) {
        NSLog(@"Exception in connectButtonClicked: %@", exception);
    }
}
@end

#endif

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

#ifdef USE_METAL_BACKEND
    // Setup titlebar after window creation
    setupTitlebar();
    std::cout << "Application initialized successfully (with Metal backend)" << std::endl;
#else
    std::cout << "Application initialized successfully (with OpenGL backend)" << std::endl;
#endif
    return true;
}

#ifdef USE_METAL_BACKEND
void Application::setupTitlebar() {
    // Get the native NSWindow from GLFW
    NSWindow *nsWindow = glfwGetCocoaWindow(window);
    if (!nsWindow) {
        std::cerr << "Failed to get NSWindow from GLFW" << std::endl;
        return;
    }

    // Make titlebar transparent and extend content under it
    nsWindow.titlebarAppearsTransparent = YES;

    // Add unified titlebar and full size content view to increase height
    [nsWindow setStyleMask:[nsWindow styleMask]];

    // Create and add a toolbar to increase titlebar height
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"MainToolbar"];
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.showsBaselineSeparator = NO;

    // Set up toolbar delegate - keep strong reference to prevent deallocation
    static ToolbarDelegate *toolbarDelegate = [[ToolbarDelegate alloc] init];
    toolbarDelegate.app = this;
    toolbar.delegate = toolbarDelegate;

    [nsWindow setToolbar:toolbar];

    // Set background color to match app theme
    const auto &colors = darkTheme ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    NSColor *bgColor = [NSColor colorWithRed:colors.base.x
                                       green:colors.base.y
                                        blue:colors.base.z
                                       alpha:colors.base.w];
    [nsWindow setBackgroundColor:bgColor];
    //    [nsWindow setBackgroundColor:[NSColor clearColor]];

    // Keep title visible and draggable
    //     nsWindow.titleVisibility = NSWindowTitleHidden;

    std::cout << "Titlebar configured successfully" << std::endl;
}

float Application::getTitlebarHeight() const {
    NSWindow *nsWindow = glfwGetCocoaWindow(window);
    if (!nsWindow) {
        return 0.0f;
    }

    // Get the titlebar height
    NSRect frame = [nsWindow frame];
    NSRect contentRect = [nsWindow contentRectForFrameRect:frame];
    return frame.size.height - contentRect.size.height;
}

void Application::onConnectButtonClicked() {
    std::cout << "Connect button clicked" << std::endl;
    try {
        // Show the connection dialog
        if (databaseSidebar) {
            std::cout << "Showing connection dialog" << std::endl;
            databaseSidebar->showConnectionDialog();
        } else {
            std::cerr << "DatabaseSidebar is null" << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << "Exception in onConnectButtonClicked: " << e.what() << std::endl;
    }
}
#endif

void Application::run() {
#ifdef USE_OPENGL_BACKEND
    glClearColor(darkTheme ? 0.110f : 0.957f, darkTheme ? 0.110f : 0.957f,
                 darkTheme ? 0.137f : 0.957f, 0.98f);
#endif

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

#ifdef USE_METAL_BACKEND
        @autoreleasepool {
            // Get the Metal drawable
            id<CAMetalDrawable> drawable = [(CAMetalLayer *)metalLayer nextDrawable];
            if (!drawable) {
                continue;
            }

            // Create render pass descriptor
            MTLRenderPassDescriptor *renderPassDescriptor =
                [MTLRenderPassDescriptor renderPassDescriptor];
            renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
            renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
            renderPassDescriptor.colorAttachments[0].clearColor =
                MTLClearColorMake(darkTheme ? 0.110f : 0.957f, darkTheme ? 0.110f : 0.957f,
                                  darkTheme ? 0.137f : 0.957f, 1.0f);
            renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

            // Create command buffer
            id<MTLCommandBuffer> commandBuffer =
                [(id<MTLCommandQueue>)metalCommandQueue commandBuffer];

            // Create render command encoder
            id<MTLRenderCommandEncoder> renderEncoder =
                [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];

            ImGui_ImplMetal_NewFrame(renderPassDescriptor);
#elif defined(USE_OPENGL_BACKEND)
        ImGui_ImplOpenGL3_NewFrame();
#endif
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            renderMainUI();

            ImGui::Render();

            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);

#ifdef USE_METAL_BACKEND
            // Update Metal layer drawable size
            ((CAMetalLayer *)metalLayer).drawableSize = CGSizeMake(display_w, display_h);

            // Render ImGui draw data
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

            // End encoding and present
            [renderEncoder endEncoding];
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
#elif defined(USE_OPENGL_BACKEND)
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

    // Cleanup ImGui - follow the order from ImGui GLFW+Metal example
#ifdef USE_METAL_BACKEND
    ImGui_ImplMetal_Shutdown();
    std::cout << "ImGui Metal backend shutdown" << std::endl;
#elif defined(USE_OPENGL_BACKEND)
    ImGui_ImplOpenGL3_Shutdown();
    std::cout << "ImGui OpenGL backend shutdown" << std::endl;
#endif
    ImGui_ImplGlfw_Shutdown();
    std::cout << "ImGui GLFW backend shutdown" << std::endl;
    ImGui::DestroyContext();
    std::cout << "ImGui context destroyed" << std::endl;

    // Cleanup GLFW
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

void Application::restorePreviousConnections() {
    if (!appState) {
        return;
    }

    auto savedConnections = appState->getSavedConnections();
    std::cout << "Restoring " << savedConnections.size() << " previous connections..." << std::endl;

    for (const auto &conn : savedConnections) {
        std::shared_ptr<DatabaseInterface> db = nullptr;

        if (conn.type == "postgresql") {
            db = std::make_shared<PostgreSQLDatabase>(conn.name, conn.host, conn.port,
                                                      conn.database, conn.username, "password");
        } else if (conn.type == "sqlite") {
            db = std::make_shared<SQLiteDatabase>(conn.name, conn.path);
        }

        if (db) {
            // Try to connect (will fail for PostgreSQL without password, but that's expected)
            auto [success, error] = db->connect();
            if (success) {
                db->refreshTables();
                std::cout << "Successfully restored connection: " << conn.name << std::endl;
            } else {
                std::cout << "Added connection (requires authentication): " << conn.name
                          << std::endl;
            }

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

#ifdef USE_METAL_BACKEND
    // Initialize Metal device and layer
    metalDevice = MTLCreateSystemDefaultDevice();
    if (!metalDevice) {
        std::cerr << "Failed to create Metal device" << std::endl;
        return false;
    }

    metalCommandQueue = [(id<MTLDevice>)metalDevice newCommandQueue];
    if (!metalCommandQueue) {
        std::cerr << "Failed to create Metal command queue" << std::endl;
        return false;
    }

    // Set up Metal layer
    NSWindow *nsWindow = glfwGetCocoaWindow(window);
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = (id<MTLDevice>)metalDevice;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = YES; // vsync
    nsWindow.contentView.layer = layer;
    nsWindow.contentView.wantsLayer = YES;
    metalLayer = layer;

    std::cout << "Metal device and layer initialized successfully" << std::endl;
#elif defined(USE_OPENGL_BACKEND)
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

#ifdef USE_METAL_BACKEND
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init((id<MTLDevice>)metalDevice);
    std::cout << "ImGui initialized with Metal backend" << std::endl;
#elif defined(USE_OPENGL_BACKEND)
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    std::cout << "ImGui initialized with OpenGL backend" << std::endl;
#endif

    return true;
}

void Application::setupFonts() {
    ImGuiIO &io = ImGui::GetIO();

    // Configure fonts with Japanese support
    ImFontConfig fontConfig;
    fontConfig.MergeMode = false;

    bool fontLoaded = false;

    // Try to load embedded fonts first
    size_t embeddedFontCount = getEmbeddedFontCount();
    if (embeddedFontCount > 0) {
        const EmbeddedFont *embeddedFonts = getEmbeddedFonts();
        for (size_t i = 0; i < embeddedFontCount; ++i) {
            const EmbeddedFont &font = embeddedFonts[i];

            // Choose appropriate glyph ranges based on font name
            const ImWchar *ranges = nullptr;
            std::string fontName = font.name;

            if (fontName.find("CJK") != std::string::npos ||
                fontName.find("Han") != std::string::npos) {
                continue; // Skip CJK fonts to avoid memory issues
            } else if (fontName.find("Cyrillic") != std::string::npos) {
                ranges = io.Fonts->GetGlyphRangesCyrillic();
            } else {
                ranges =
                    io.Fonts->GetGlyphRangesDefault(); // Use default ranges instead of Japanese
            }

            // Create a copy of fontConfig for each font to avoid reuse issues
            ImFontConfig embeddedFontConfig = fontConfig;
            embeddedFontConfig.FontDataOwnedByAtlas =
                false; // Don't let ImGui free the embedded data
            ImFont *loadedFont = io.Fonts->AddFontFromMemoryTTF((void *)font.data, (int)font.size,
                                                                16.0f, &embeddedFontConfig, ranges);

            if (loadedFont) {
                std::cout << "✓ Successfully loaded embedded font: " << font.name << std::endl;
                fontLoaded = true;
                break; // Use the first successfully loaded font
            }
        }
    }

    // Try to load fonts from assets folder first, then fallback to system fonts
    std::vector<std::string> fontPaths = {// Asset fonts first
                                          "assets/fonts/NotoSans-Regular.ttf",
                                          "assets/fonts/NotoSansCJK-Regular.otf",
// System fonts as fallback
#ifdef __APPLE__
                                          "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                          "/System/Library/Fonts/PingFang.ttc",
                                          "/System/Library/Fonts/Helvetica.ttc",
                                          "/Library/Fonts/Arial Unicode MS.ttf"
#elif defined(_WIN32)
                                          "C:/Windows/Fonts/msgothic.ttc",
                                          "C:/Windows/Fonts/meiryo.ttc",
                                          "C:/Windows/Fonts/YuGothM.ttc",
                                          "C:/Windows/Fonts/arial.ttf"
#else
                                          "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                                          "/usr/share/fonts/truetype/liberation/"
                                          "LiberationSans-Regular.ttf",
                                          "/usr/share/fonts/TTF/DejaVuSans.ttf"
#endif
    };

    // Try to load the first available font
    for (const auto &fontPath : fontPaths) {
        std::ifstream fontFile(fontPath);
        if (fontFile.good()) {
            fontFile.close();

            // Load the font with comprehensive Unicode ranges
            const ImWchar *ranges = nullptr;

            // Choose appropriate glyph ranges based on font name
            if (fontPath.find("Cyrillic") != std::string::npos) {
                ranges = io.Fonts->GetGlyphRangesCyrillic(); // Cyrillic + Latin
            } else if (fontPath.find("unifont") != std::string::npos) {
                ranges = nullptr; // Load all available glyphs for maximum coverage
            } else {
                // For general fonts, use default ranges
                ranges = io.Fonts->GetGlyphRangesDefault();
            }

            ImFont *font =
                io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &fontConfig, ranges);

            if (font != nullptr) {
                if (fontPath.find("assets/fonts/") == 0) {
                    std::cout << "✓ Successfully loaded custom font: " << fontPath << std::endl;
                } else {
                    std::cout << "✓ Successfully loaded system font: " << fontPath << std::endl;
                }
                fontLoaded = true;
                break;
            }
        }
    }

    // Fallback: Use default font with Japanese ranges
    if (!fontLoaded) {
        std::cout << "⚠ No custom or system fonts found, using default font with Japanese ranges"
                  << std::endl;
        fontConfig.MergeMode = false;
        io.Fonts->AddFontDefault(&fontConfig);
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

    ImGuiWindowFlags window_flags =
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

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));

    // Setup default docking layout
    setupDockingLayout(dockspace_id);

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
