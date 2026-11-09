#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include "application.hpp"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "platform/ios_platform.hpp"

#include <spdlog/spdlog.h>

namespace {
    MTKView* gView = nil;
    id<MTLCommandQueue> gQueue = nil;
    CFTimeInterval gLastTime = 0;
} // namespace

// ---- the view: touches and the ipad pointer become imgui mouse events ----

@interface DearSQLView : MTKView
@end

@implementation DearSQLView
- (void)sendTouches:(UIEvent*)event {
    ImGuiIO& io = ImGui::GetIO();
    UITouch* touch = event.allTouches.anyObject;
    if (!touch)
        return;
    const CGPoint p = [touch locationInView:self];
    io.AddMouseSourceEvent(touch.type == UITouchTypeIndirectPointer ? ImGuiMouseSource_Mouse
                                                                    : ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(p.x, p.y);
    bool down = false;
    for (UITouch* t in event.allTouches)
        down |= t.phase != UITouchPhaseEnded && t.phase != UITouchPhaseCancelled;
    io.AddMouseButtonEvent(0, down);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self sendTouches:event];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self sendTouches:event];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self sendTouches:event];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self sendTouches:event];
}
@end

// ---- the controller: owns the view, starts the app, draws each frame ----

@interface DearSQLViewController : UIViewController <MTKViewDelegate>
@end

@implementation DearSQLViewController
- (void)loadView {
    DearSQLView* view = [[DearSQLView alloc] initWithFrame:UIScreen.mainScreen.bounds
                                                    device:MTLCreateSystemDefaultDevice()];
    view.delegate = self;
    view.preferredFramesPerSecond = 120;
    view.multipleTouchEnabled = YES;
    // pointer hover (trackpad, mouse) moves the imgui cursor without a click
    [view addGestureRecognizer:[[UIHoverGestureRecognizer alloc] initWithTarget:self
                                                                         action:@selector(hover:)]];
    // two fingers scroll, like a trackpad
    UIPanGestureRecognizer* pan = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                                          action:@selector(pan:)];
    pan.minimumNumberOfTouches = 2;
    pan.allowedScrollTypesMask = UIScrollTypeMaskAll;
    [view addGestureRecognizer:pan];
    self.view = view;
    gView = view;
    gQueue = [view.device newCommandQueue];

    if (!Application::getInstance().initialize())
        spdlog::error("ios: application failed to initialize");
    // like `dearsql <file>` on the desktop; the simulator passes SIMCTL_CHILD_DEARSQL_OPEN
    if (const char* path = getenv("DEARSQL_OPEN"))
        Application::getInstance().openFile(path);
}
- (void)hover:(UIHoverGestureRecognizer*)g {
    const CGPoint p = [g locationInView:self.view];
    ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    ImGui::GetIO().AddMousePosEvent(p.x, p.y);
}
- (void)pan:(UIPanGestureRecognizer*)g {
    const CGPoint d = [g translationInView:self.view];
    [g setTranslation:CGPointZero inView:self.view];
    ImGui::GetIO().AddMouseWheelEvent(d.x / 20.0f, d.y / 20.0f);
}
- (void)drawInMTKView:(MTKView*)view {
    Application::getInstance().getPlatform()->renderFrame();
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}
- (BOOL)prefersStatusBarHidden {
    return NO;
}
@end

// ---- scene and app delegates ----

@interface DearSQLSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation DearSQLSceneDelegate
- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [[DearSQLViewController alloc] init];
    [self.window makeKeyAndVisible];
}
@end

@interface DearSQLAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation DearSQLAppDelegate
- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* config = [[UISceneConfiguration alloc] initWithName:@"Default"
                                                                  sessionRole:session.role];
    config.delegateClass = DearSQLSceneDelegate.class;
    return config;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        // ~/.dearsql lands in the container's Library: its root is read-only on a device
        setenv("HOME", [NSHomeDirectory() stringByAppendingPathComponent:@"Library"].UTF8String, 1);
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(DearSQLAppDelegate.class));
    }
}

// ---- IOSPlatform ----

bool IOSPlatform::initializeImGuiBackend() {
    ImGui::GetIO().BackendPlatformName = "dearsql_ios";
    return ImGui_ImplMetal_Init(gView.device);
}

void IOSPlatform::onSidebarToggleClicked() {
    app_->setSidebarVisible(!app_->isSidebarVisible());
}

float IOSPlatform::getClientAreaTopInset() const {
    return gView ? static_cast<float>(gView.safeAreaInsets.top) : 0.0f;
}

void IOSPlatform::renderFrame() {
    @autoreleasepool {
        ImGuiIO& io = ImGui::GetIO();
        const CGSize size = gView.bounds.size;
        const CGFloat scale = gView.contentScaleFactor;
        io.DisplaySize = ImVec2(size.width, size.height);
        io.DisplayFramebufferScale = ImVec2(scale, scale);
        const CFTimeInterval now = CACurrentMediaTime();
        io.DeltaTime = gLastTime > 0 ? static_cast<float>(now - gLastTime) : 1.0f / 60.0f;
        gLastTime = now;

        MTLRenderPassDescriptor* pass = gView.currentRenderPassDescriptor;
        if (!pass)
            return;
        const auto& base = app_->getCurrentColors().base;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(base.x, base.y, base.z, 1);
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;

        id<MTLCommandBuffer> commands = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        ImGui_ImplMetal_NewFrame(pass);
        ImGui::NewFrame();
        app_->renderMainUI();
        ImGui::Render();
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commands, encoder);
        [encoder endEncoding];
        [commands presentDrawable:gView.currentDrawable];
        [commands commit];
    }
}

void IOSPlatform::shutdownImGui() {
    ImGui_ImplMetal_Shutdown();
}

ImTextureID IOSPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    if (!gView || !pixels)
        return ImTextureID{};
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [gView.device newTextureWithDescriptor:desc];
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:width * 4];
    // imgui holds the id, not a reference: keep the texture alive with the app
    return (ImTextureID)(intptr_t)(__bridge_retained void*)texture;
}
