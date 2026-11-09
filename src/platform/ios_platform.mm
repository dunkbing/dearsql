#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include "application.hpp"
#include "database/connection_url.hpp"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "platform/ios_platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <string>

namespace {
    MTKView* gView = nil;
    id<MTLCommandQueue> gQueue = nil;
    CFTimeInterval gLastTime = 0;
    CGFloat gKeyboardInset = 0; // how much of the view the soft keyboard covers
} // namespace

// ---- the view: touches, the ipad pointer and keys become imgui input ----

namespace {
    // a finger touch is held back until we know what it is: a tap (click), a drag on
    // something draggable (press and drag), a scroll (pan) or a long press (right click)
    enum class Touch { None, Pending, Pressed };
    Touch gTouch = Touch::None;
    CGPoint gTouchStart;

    ImGuiKey keyFor(UIKeyboardHIDUsage u) {
        if (u >= UIKeyboardHIDUsageKeyboardA && u <= UIKeyboardHIDUsageKeyboardZ)
            return static_cast<ImGuiKey>(ImGuiKey_A + (u - UIKeyboardHIDUsageKeyboardA));
        if (u >= UIKeyboardHIDUsageKeyboard1 && u <= UIKeyboardHIDUsageKeyboard9)
            return static_cast<ImGuiKey>(ImGuiKey_1 + (u - UIKeyboardHIDUsageKeyboard1));
        if (u >= UIKeyboardHIDUsageKeyboardF1 && u <= UIKeyboardHIDUsageKeyboardF12)
            return static_cast<ImGuiKey>(ImGuiKey_F1 + (u - UIKeyboardHIDUsageKeyboardF1));
        switch (u) {
        case UIKeyboardHIDUsageKeyboard0:
            return ImGuiKey_0;
        case UIKeyboardHIDUsageKeyboardReturnOrEnter:
            return ImGuiKey_Enter;
        case UIKeyboardHIDUsageKeypadEnter:
            return ImGuiKey_KeypadEnter;
        case UIKeyboardHIDUsageKeyboardEscape:
            return ImGuiKey_Escape;
        case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
            return ImGuiKey_Backspace;
        case UIKeyboardHIDUsageKeyboardDeleteForward:
            return ImGuiKey_Delete;
        case UIKeyboardHIDUsageKeyboardTab:
            return ImGuiKey_Tab;
        case UIKeyboardHIDUsageKeyboardSpacebar:
            return ImGuiKey_Space;
        case UIKeyboardHIDUsageKeyboardHome:
            return ImGuiKey_Home;
        case UIKeyboardHIDUsageKeyboardEnd:
            return ImGuiKey_End;
        case UIKeyboardHIDUsageKeyboardPageUp:
            return ImGuiKey_PageUp;
        case UIKeyboardHIDUsageKeyboardPageDown:
            return ImGuiKey_PageDown;
        case UIKeyboardHIDUsageKeyboardLeftArrow:
            return ImGuiKey_LeftArrow;
        case UIKeyboardHIDUsageKeyboardRightArrow:
            return ImGuiKey_RightArrow;
        case UIKeyboardHIDUsageKeyboardUpArrow:
            return ImGuiKey_UpArrow;
        case UIKeyboardHIDUsageKeyboardDownArrow:
            return ImGuiKey_DownArrow;
        case UIKeyboardHIDUsageKeyboardSlash:
            return ImGuiKey_Slash;
        case UIKeyboardHIDUsageKeyboardLeftControl:
            return ImGuiKey_LeftCtrl;
        case UIKeyboardHIDUsageKeyboardRightControl:
            return ImGuiKey_RightCtrl;
        case UIKeyboardHIDUsageKeyboardLeftShift:
            return ImGuiKey_LeftShift;
        case UIKeyboardHIDUsageKeyboardRightShift:
            return ImGuiKey_RightShift;
        case UIKeyboardHIDUsageKeyboardLeftAlt:
            return ImGuiKey_LeftAlt;
        case UIKeyboardHIDUsageKeyboardRightAlt:
            return ImGuiKey_RightAlt;
        case UIKeyboardHIDUsageKeyboardLeftGUI:
            return ImGuiKey_LeftSuper;
        case UIKeyboardHIDUsageKeyboardRightGUI:
            return ImGuiKey_RightSuper;
        default:
            return ImGuiKey_None;
        }
    }

    // keys whose text comes through insertText:, so uikit must still see them
    bool typesText(ImGuiKey k) {
        return k == ImGuiKey_None || k == ImGuiKey_Space || k == ImGuiKey_Slash ||
               (k >= ImGuiKey_0 && k <= ImGuiKey_9) || (k >= ImGuiKey_A && k <= ImGuiKey_Z);
    }

    void tapKey(ImGuiKey k) {
        ImGui::GetIO().AddKeyEvent(k, true);
        ImGui::GetIO().AddKeyEvent(k, false);
    }

    void touchAt(CGPoint p) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(p.x, p.y);
    }

    // let go without clicking whatever is under the finger
    void releaseAway() {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
    }

    UIView* gNoKeyboard = nil; // an empty input view: first responder, no keyboard
} // namespace

@interface DearSQLView : MTKView <UIKeyInput>
@property(nonatomic) BOOL keyboardWanted;
@end

@implementation DearSQLView
- (BOOL)canBecomeFirstResponder {
    return YES; // always: hardware keys and shortcuts arrive even with no text field active
}
- (UIView*)inputView {
    if (!gNoKeyboard)
        gNoKeyboard = [[UIView alloc] initWithFrame:CGRectZero];
    return self.keyboardWanted ? nil : gNoKeyboard;
}
- (BOOL)hasText {
    return YES;
}
- (void)insertText:(NSString*)text {
    if ([text isEqualToString:@"\n"])
        tapKey(ImGuiKey_Enter);
    else
        ImGui::GetIO().AddInputCharactersUTF8(text.UTF8String);
}
- (void)deleteBackward {
    tapKey(ImGuiKey_Backspace);
}
// sql, not prose
- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}
- (UITextSpellCheckingType)spellCheckingType {
    return UITextSpellCheckingTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartDashesType)smartDashesType {
    return UITextSmartDashesTypeNo;
}
- (UIKeyboardType)keyboardType {
    return UIKeyboardTypeASCIICapable;
}

- (void)sendPresses:(NSSet<UIPress*>*)presses down:(BOOL)down event:(UIPressesEvent*)event {
    ImGuiIO& io = ImGui::GetIO();
    NSMutableSet<UIPress*>* text = [NSMutableSet set];
    for (UIPress* press in presses) {
        if (!press.key) {
            [text addObject:press];
            continue;
        }
        const UIKeyModifierFlags mods = press.key.modifierFlags;
        const ImGuiKey key = keyFor(press.key.keyCode);
        switch (key) {
        case ImGuiKey_LeftCtrl:
        case ImGuiKey_RightCtrl:
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            break;
        case ImGuiKey_LeftShift:
        case ImGuiKey_RightShift:
            io.AddKeyEvent(ImGuiMod_Shift, down);
            break;
        case ImGuiKey_LeftAlt:
        case ImGuiKey_RightAlt:
            io.AddKeyEvent(ImGuiMod_Alt, down);
            break;
        case ImGuiKey_LeftSuper:
        case ImGuiKey_RightSuper:
            io.AddKeyEvent(ImGuiMod_Super, down);
            break;
        default:
            break;
        }
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, down);
        // ⌘C and friends are shortcuts, not text
        if (typesText(key) && !(mods & (UIKeyModifierCommand | UIKeyModifierControl)))
            [text addObject:press];
    }
    if (text.count == 0)
        return;
    if (down)
        [super pressesBegan:text withEvent:event];
    else
        [super pressesEnded:text withEvent:event];
}
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self sendPresses:presses down:YES event:event];
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self sendPresses:presses down:NO event:event];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self sendPresses:presses down:NO event:event];
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    const CGPoint p = [touch locationInView:self];
    ImGuiIO& io = ImGui::GetIO();
    if (touch.type == UITouchTypeIndirectPointer) { // trackpad or mouse: a mouse
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent((event.buttonMask & UIEventButtonMaskSecondary) ? 1 : 0, true);
        return;
    }
    if (gTouch != Touch::None)
        return; // one finger drives the mouse
    gTouch = Touch::Pending;
    gTouchStart = p;
    touchAt(p); // hover only: the cursor shape tells the pan what lies under the finger
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    const CGPoint p = [touch locationInView:self];
    if (touch.type == UITouchTypeIndirectPointer) {
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
        return;
    }
    if (gTouch == Touch::Pressed)
        touchAt(p);
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    ImGuiIO& io = ImGui::GetIO();
    if (touch.type == UITouchTypeIndirectPointer) {
        io.AddMouseButtonEvent(0, false);
        io.AddMouseButtonEvent(1, false);
        return;
    }
    if (gTouch == Touch::Pending) { // a tap
        touchAt(gTouchStart);
        io.AddMouseButtonEvent(0, true);
    }
    if (gTouch != Touch::None)
        io.AddMouseButtonEvent(0, false);
    gTouch = Touch::None;
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (touches.anyObject.type == UITouchTypeIndirectPointer) {
        releaseAway();
        return;
    }
    if (gTouch == Touch::Pressed)
        releaseAway();
    gTouch = Touch::None;
}
@end

// ---- the controller: owns the view, starts the app, draws each frame ----

@interface DearSQLViewController : UIViewController <MTKViewDelegate, UIGestureRecognizerDelegate>
@end

@implementation DearSQLViewController {
    UIPanGestureRecognizer* pan_;
    UILongPressGestureRecognizer* longPress_;
}
- (void)loadView {
    DearSQLView* view = [[DearSQLView alloc] initWithFrame:UIScreen.mainScreen.bounds
                                                    device:MTLCreateSystemDefaultDevice()];
    view.delegate = self;
    view.preferredFramesPerSecond = 120;
    view.multipleTouchEnabled = YES;
    // pointer hover (trackpad, mouse) moves the imgui cursor without a click
    [view addGestureRecognizer:[[UIHoverGestureRecognizer alloc] initWithTarget:self
                                                                         action:@selector(hover:)]];
    // a finger drag (or two, or a trackpad swipe) scrolls
    pan_ = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(pan:)];
    pan_.allowedScrollTypesMask = UIScrollTypeMaskAll;
    pan_.delegate = self;
    [view addGestureRecognizer:pan_];
    // long press = right click (context menus)
    longPress_ = [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                               action:@selector(longPress:)];
    longPress_.delegate = self;
    [view addGestureRecognizer:longPress_];
    self.view = view;
    gView = view;
    gQueue = [view.device newCommandQueue];

    if (!Application::getInstance().initialize())
        spdlog::error("ios: application failed to initialize");
    // like `dearsql <file>` on the desktop; the simulator passes SIMCTL_CHILD_DEARSQL_OPEN.
    // a connection url opens unsaved, for trying a server from the simulator
    if (const char* open = getenv("DEARSQL_OPEN")) {
        auto& app = Application::getInstance();
        if (auto url = parseConnectionUrl(open); url.ok) {
            if (url.info.name.empty())
                url.info.name = url.info.host + ":" + std::to_string(url.info.port);
            auto db = DatabaseFactory::createDatabase(url.info);
            app.addDatabase(db);
            app.setSelectedDatabase(db);
            db->startConnectionAsync();
        } else {
            app.openFile(open);
        }
    }
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self.view becomeFirstResponder];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(keyboardFrame:)
                                               name:UIKeyboardWillChangeFrameNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(keyboardFrame:)
                                               name:UIKeyboardWillHideNotification
                                             object:nil];
}
// the ui lays out above the keyboard (a floating or undocked one covers nothing)
- (void)keyboardFrame:(NSNotification*)note {
    if ([note.name isEqualToString:UIKeyboardWillHideNotification]) {
        gKeyboardInset = 0;
        return;
    }
    const CGRect screen = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    const CGRect local = [self.view convertRect:screen
                            fromCoordinateSpace:self.view.window.screen.coordinateSpace];
    const CGRect covered = CGRectIntersection(self.view.bounds, local);
    gKeyboardInset =
        CGRectIsNull(covered) || CGRectGetMaxY(covered) < CGRectGetMaxY(self.view.bounds)
            ? 0
            : covered.size.height;
}
// gestures are for fingers; a trackpad click or drag is a mouse
- (BOOL)gestureRecognizer:(UIGestureRecognizer*)g shouldReceiveTouch:(UITouch*)touch {
    return touch.type == UITouchTypeDirect;
}
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer*)g {
    if (g != pan_ || g.numberOfTouches != 1)
        return YES; // long press, two fingers, trackpad scroll
    // one finger on a splitter, column edge or text: drag it instead of scrolling
    const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
    if (cursor == ImGuiMouseCursor_Arrow || cursor == ImGuiMouseCursor_Hand)
        return YES;
    gTouch = Touch::Pressed;
    touchAt(gTouchStart);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    touchAt([g locationInView:self.view]);
    return NO;
}
- (void)hover:(UIHoverGestureRecognizer*)g {
    const CGPoint p = [g locationInView:self.view];
    ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    ImGui::GetIO().AddMousePosEvent(p.x, p.y);
}
- (void)pan:(UIPanGestureRecognizer*)g {
    const CGPoint d = [g translationInView:self.view];
    [g setTranslation:CGPointZero inView:self.view];
    if (g.numberOfTouches > 0)
        touchAt([g locationInView:self.view]); // scroll the window under the finger
    // imgui scrolls 5 lines per wheel step: this keeps the content under the finger
    const float step = 5.0f * std::max(ImGui::GetFontSize(), 1.0f);
    ImGui::GetIO().AddMouseWheelEvent(d.x / step, d.y / step);
}
- (void)longPress:(UILongPressGestureRecognizer*)g {
    if (g.state != UIGestureRecognizerStateBegan)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (gTouch == Touch::Pressed)
        releaseAway();
    gTouch = Touch::None;
    touchAt([g locationInView:self.view]);
    io.AddMouseButtonEvent(1, true);
    io.AddMouseButtonEvent(1, false);
}
- (void)drawInMTKView:(MTKView*)view {
    Application::getInstance().getPlatform()->renderFrame();
    // show the soft keyboard while a text field wants it
    DearSQLView* v = (DearSQLView*)view;
    const BOOL want = ImGui::GetIO().WantTextInput;
    if (want != v.keyboardWanted) {
        v.keyboardWanted = want;
        [v reloadInputViews];
    }
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
    ImGui::GetIO().MouseDoubleClickMaxDist = 20.0f; // fingers land less precisely than a mouse
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        static std::string text;
        text = UIPasteboard.generalPasteboard.string.UTF8String ?: "";
        return text.c_str();
    };
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        UIPasteboard.generalPasteboard.string = [NSString stringWithUTF8String:text];
    };
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
        io.DisplaySize = ImVec2(size.width, std::max<CGFloat>(size.height - gKeyboardInset, 1));
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
