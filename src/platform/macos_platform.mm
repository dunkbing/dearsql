#include "platform/macos_platform.hpp"
#include "application.hpp"
#include "themes.hpp"
#include <iostream>

#ifdef USE_METAL_BACKEND
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#import "imgui_impl_metal.h"
#import <GLFW/glfw3native.h>

// Toolbar delegate interface
@interface ToolbarDelegate : NSObject <NSToolbarDelegate>
@property(nonatomic, assign) Application *app;
@end

@implementation ToolbarDelegate
- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar {
    return @[ @"ConnectButton" ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    return @[ @"SidebarToggle", @"ConnectButton", NSToolbarFlexibleSpaceItemIdentifier ];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)itemIdentifier
    willBeInsertedIntoToolbar:(BOOL)flag {
    if ([itemIdentifier isEqualToString:@"SidebarToggle"]) {
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:itemIdentifier];
        item.label = @"";
        item.paletteLabel = @"Toggle Sidebar";
        item.toolTip = @"Show/Hide Sidebar";

        NSButton *button = [[NSButton alloc] init];
        [button setImage:[NSImage imageWithSystemSymbolName:@"sidebar.left"
                                   accessibilityDescription:@"Toggle Sidebar"]];
        [button setButtonType:NSButtonTypeMomentaryPushIn];
        [button setBezelStyle:NSBezelStyleRounded];
        [button setTarget:self];
        [button setAction:@selector(sidebarToggleClicked:)];
        [button sizeToFit];

        item.view = button;
        return item;
    } else if ([itemIdentifier isEqualToString:@"ConnectButton"]) {
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

- (void)sidebarToggleClicked:(id)sender {
    @try {
        if (self.app) {
            self.app->onSidebarToggleClicked();
        }
    } @catch (NSException *exception) {
        NSLog(@"Exception in sidebarToggleClicked: %@", exception);
    }
}
@end

#endif

MacOSPlatform::MacOSPlatform(Application *app) : app_(app), window_(nullptr) {
#ifdef USE_METAL_BACKEND
    toolbarDelegate_ = nullptr;
    metalDevice_ = nullptr;
    metalCommandQueue_ = nullptr;
    metalLayer_ = nullptr;
#endif
}

MacOSPlatform::~MacOSPlatform() {
    cleanup();
}

bool MacOSPlatform::initializePlatform(GLFWwindow *window) {
    window_ = window;

#ifdef USE_METAL_BACKEND
    // Initialize Metal device and layer
    metalDevice_ = MTLCreateSystemDefaultDevice();
    if (!metalDevice_) {
        std::cerr << "Failed to create Metal device" << std::endl;
        return false;
    }

    metalCommandQueue_ = [(id<MTLDevice>)metalDevice_ newCommandQueue];
    if (!metalCommandQueue_) {
        std::cerr << "Failed to create Metal command queue" << std::endl;
        return false;
    }

    // Set up Metal layer
    NSWindow *nsWindow = glfwGetCocoaWindow(window);
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = (id<MTLDevice>)metalDevice_;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = YES; // vsync
    nsWindow.contentView.layer = layer;
    nsWindow.contentView.wantsLayer = YES;
    metalLayer_ = layer;

    std::cout << "Metal device and layer initialized successfully" << std::endl;
#endif

    return true;
}

bool MacOSPlatform::initializeImGuiBackend() {
#ifdef USE_METAL_BACKEND
    ImGui_ImplMetal_Init((id<MTLDevice>)metalDevice_);
    std::cout << "ImGui Metal backend initialized" << std::endl;
#endif
    return true;
}

void MacOSPlatform::setupTitlebar() {
#ifdef USE_METAL_BACKEND
    // Get the native NSWindow from GLFW
    NSWindow *nsWindow = glfwGetCocoaWindow(window_);
    if (!nsWindow) {
        std::cerr << "Failed to get NSWindow from GLFW" << std::endl;
        return;
    }

    // Make titlebar transparent and extend content under it
    nsWindow.titlebarAppearsTransparent = YES;

    // Add unified titlebar and full size content view to increase height
    [nsWindow setStyleMask:[nsWindow styleMask]];

    // Set up toolbar delegate first - keep strong reference to prevent deallocation
    toolbarDelegate_ = [[ToolbarDelegate alloc] init];
    toolbarDelegate_.app = app_;

    // Create custom title bar accessory view for sidebar button aligned with toolbar
    NSButton *sidebarButton = [[NSButton alloc] initWithFrame:NSMakeRect(0, 12, 40, 30)];
    [sidebarButton setImage:[NSImage imageWithSystemSymbolName:@"sidebar.left"
                                      accessibilityDescription:@"Toggle Sidebar"]];
    [sidebarButton setButtonType:NSButtonTypeMomentaryPushIn];
    [sidebarButton setBezelStyle:NSBezelStyleTexturedRounded];
    [sidebarButton setTarget:toolbarDelegate_];
    [sidebarButton setAction:@selector(sidebarToggleClicked:)];
    [sidebarButton setBordered:NO];

    NSTitlebarAccessoryViewController *accessoryController =
        [[NSTitlebarAccessoryViewController alloc] init];
    accessoryController.view = sidebarButton;
    accessoryController.layoutAttribute = NSLayoutAttributeLeading;

    [nsWindow addTitlebarAccessoryViewController:accessoryController];

    // Connect button
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"MainToolbar"];
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    toolbar.delegate = toolbarDelegate_;

    [nsWindow setToolbar:toolbar];

    std::cout << "Custom titlebar accessory view created for sidebar toggle next to title"
              << std::endl;

    // Set background color to match app theme
    const auto &colors = app_->isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    NSColor *bgColor = [NSColor colorWithRed:colors.base.x
                                       green:colors.base.y
                                        blue:colors.base.z
                                       alpha:colors.base.w];
    [nsWindow setBackgroundColor:bgColor];

    std::cout << "Titlebar configured successfully" << std::endl;
#endif
}

float MacOSPlatform::getTitlebarHeight() const {
#ifdef USE_METAL_BACKEND
    NSWindow *nsWindow = glfwGetCocoaWindow(window_);
    if (!nsWindow) {
        return 0.0f;
    }

    // Get the titlebar height
    NSRect frame = [nsWindow frame];
    NSRect contentRect = [nsWindow contentRectForFrameRect:frame];
    return static_cast<float>(frame.size.height - contentRect.size.height);
#else
    return 0.0f;
#endif
}

void MacOSPlatform::onConnectButtonClicked() {
    std::cout << "Connect button clicked" << std::endl;
    try {
        // Show the connection dialog
        if (app_->getDatabaseSidebar()) {
            std::cout << "Showing connection dialog" << std::endl;
            app_->getDatabaseSidebar()->showConnectionDialog();
        } else {
            std::cerr << "DatabaseSidebar is null" << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << "Exception in onConnectButtonClicked: " << e.what() << std::endl;
    }
}

void MacOSPlatform::onSidebarToggleClicked() {
    std::cout << "Sidebar toggle clicked" << std::endl;
    try {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    } catch (const std::exception &e) {
        std::cerr << "Exception in onSidebarToggleClicked: " << e.what() << std::endl;
    }
}

void MacOSPlatform::cleanup() {
#ifdef USE_METAL_BACKEND
    if (toolbarDelegate_) {
        toolbarDelegate_ = nullptr;
    }
    metalDevice_ = nullptr;
    metalCommandQueue_ = nullptr;
    metalLayer_ = nullptr;
#endif
}

void MacOSPlatform::renderFrame() {
#ifdef USE_METAL_BACKEND
    @autoreleasepool {
        // Get the Metal drawable
        id<CAMetalDrawable> drawable = [(CAMetalLayer *)metalLayer_ nextDrawable];
        if (!drawable) {
            return;
        }

        // Create render pass descriptor
        MTLRenderPassDescriptor *renderPassDescriptor =
            [MTLRenderPassDescriptor renderPassDescriptor];
        renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
        renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(
            app_->isDarkTheme() ? 0.110f : 0.957f, app_->isDarkTheme() ? 0.110f : 0.957f,
            app_->isDarkTheme() ? 0.137f : 0.957f, 1.0f);
        renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

        // Create command buffer
        id<MTLCommandBuffer> commandBuffer =
            [(id<MTLCommandQueue>)metalCommandQueue_ commandBuffer];

        // Create render command encoder
        id<MTLRenderCommandEncoder> renderEncoder =
            [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];

        ImGui_ImplMetal_NewFrame(renderPassDescriptor);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app_->renderMainUI();

        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window_, &display_w, &display_h);

        // Update Metal layer drawable size
        ((CAMetalLayer *)metalLayer_).drawableSize = CGSizeMake(display_w, display_h);

        // Render ImGui draw data
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

        // End encoding and present
        [renderEncoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
#endif
}

void MacOSPlatform::shutdownImGui() {
#ifdef USE_METAL_BACKEND
    ImGui_ImplMetal_Shutdown();
    std::cout << "ImGui Metal backend shutdown" << std::endl;
#endif
}
