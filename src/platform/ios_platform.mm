#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include "application.hpp"
#include "database/connection_url.hpp"
#include "database/file_database.hpp"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "platform/ios_platform.hpp"
#include "themes.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab_manager.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <string>

namespace {
    MTKView* gView = nil;
    UINavigationController* gNav = nil;
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

// UITextInput over a document of one position: it exists for the keyboard's hold-space
// trackpad, whose floating cursor turns into arrow keys. imgui keeps the real text
// ponytail: no positions, so no loupe or selection handles; those need the real text here
@interface DearSQLTextPosition : UITextPosition
@end
@implementation DearSQLTextPosition
@end

@interface DearSQLTextRange : UITextRange
@end
@implementation DearSQLTextRange
- (UITextPosition*)start {
    static DearSQLTextPosition* p = [DearSQLTextPosition new];
    return p;
}
- (UITextPosition*)end {
    return self.start;
}
- (BOOL)isEmpty {
    return YES;
}
@end

namespace {
    DearSQLTextRange* gRange = [DearSQLTextRange new];
    CGPoint gCursor; // floating cursor, as far as arrows have moved it
    int gMarked = 0; // characters of an ime composition already sent
} // namespace

@interface DearSQLView : MTKView <UITextInput>
@property(nonatomic) BOOL keyboardWanted;
@end

@implementation DearSQLView
@synthesize inputDelegate, markedTextStyle;
- (UITextRange*)selectedTextRange {
    return gRange;
}
- (void)setSelectedTextRange:(UITextRange*)range {
}
- (UITextRange*)markedTextRange {
    return nil;
}
// a composition replaces what it sent last time
- (void)setMarkedText:(NSString*)text selectedRange:(NSRange)range {
    for (; gMarked > 0; --gMarked)
        tapKey(ImGuiKey_Backspace);
    ImGui::GetIO().AddInputCharactersUTF8(text.UTF8String);
    gMarked = (int)text.length;
}
- (void)unmarkText {
    gMarked = 0;
}
- (NSString*)textInRange:(UITextRange*)range {
    return @"";
}
- (void)replaceRange:(UITextRange*)range withText:(NSString*)text {
    [self insertText:text];
}
- (UITextPosition*)beginningOfDocument {
    return gRange.start;
}
- (UITextPosition*)endOfDocument {
    return gRange.start;
}
- (UITextRange*)textRangeFromPosition:(UITextPosition*)from toPosition:(UITextPosition*)to {
    return gRange;
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)position offset:(NSInteger)offset {
    return offset ? nil : position;
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)position
                            inDirection:(UITextLayoutDirection)direction
                                 offset:(NSInteger)offset {
    return offset ? nil : position;
}
- (NSComparisonResult)comparePosition:(UITextPosition*)a toPosition:(UITextPosition*)b {
    return NSOrderedSame;
}
- (NSInteger)offsetFromPosition:(UITextPosition*)from toPosition:(UITextPosition*)to {
    return 0;
}
- (id<UITextInputTokenizer>)tokenizer {
    return [[UITextInputStringTokenizer alloc] initWithTextInput:self];
}
- (UITextPosition*)positionWithinRange:(UITextRange*)range
                   farthestInDirection:(UITextLayoutDirection)direction {
    return gRange.start;
}
- (UITextRange*)characterRangeByExtendingPosition:(UITextPosition*)position
                                      inDirection:(UITextLayoutDirection)direction {
    return gRange;
}
- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition*)position
                                          inDirection:(UITextStorageDirection)direction {
    return NSWritingDirectionLeftToRight;
}
- (void)setBaseWritingDirection:(NSWritingDirection)direction forRange:(UITextRange*)range {
}
- (CGRect)firstRectForRange:(UITextRange*)range {
    return CGRectZero;
}
- (CGRect)caretRectForPosition:(UITextPosition*)position {
    return CGRectZero;
}
- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range {
    return @[];
}
- (UITextPosition*)closestPositionToPoint:(CGPoint)point {
    return gRange.start;
}
- (UITextPosition*)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange*)range {
    return gRange.start;
}
- (UITextRange*)characterRangeAtPoint:(CGPoint)point {
    return gRange;
}
- (void)beginFloatingCursorAtPoint:(CGPoint)point {
    gCursor = point;
}
// an arrow per character width or line the finger travels
- (void)updateFloatingCursorAtPoint:(CGPoint)point {
    const float w = ImGui::GetFontSize() * 0.6f, h = ImGui::GetTextLineHeightWithSpacing();
    for (; point.x - gCursor.x >= w; gCursor.x += w)
        tapKey(ImGuiKey_RightArrow);
    for (; gCursor.x - point.x >= w; gCursor.x -= w)
        tapKey(ImGuiKey_LeftArrow);
    for (; point.y - gCursor.y >= h; gCursor.y += h)
        tapKey(ImGuiKey_DownArrow);
    for (; gCursor.y - point.y >= h; gCursor.y -= h)
        tapKey(ImGuiKey_UpArrow);
}
- (void)endFloatingCursor {
}
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

@interface DearSQLViewController
    : UIViewController <MTKViewDelegate, UIGestureRecognizerDelegate, UIEditMenuInteractionDelegate>
@end

@implementation DearSQLViewController {
    UIPanGestureRecognizer* pan_;
    UILongPressGestureRecognizer* longPress_;
    UIEditMenuInteraction* editMenu_;
    UIBarButtonItem* sidebarItem_;
    UIBarButtonItem* newItem_;
    UIButton* workspaceButton_;
}

// ---- the bar ----

- (void)setupBar {
    sidebarItem_ = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"sidebar.left"]
                                                    style:UIBarButtonItemStylePlain
                                                   target:self
                                                   action:@selector(toggleSidebar)];
    sidebarItem_.accessibilityLabel = @"Toggle Sidebar";
    __weak DearSQLViewController* weakSelf = self;
    UIButtonConfiguration* newConfig = UIButtonConfiguration.plainButtonConfiguration;
    newConfig.image = [UIImage systemImageNamed:@"plus"];
    newConfig.title = @"New";
    newConfig.imagePadding = 4;
    newConfig.contentInsets = NSDirectionalEdgeInsetsZero;
    UIButton* newButton =
        [UIButton buttonWithConfiguration:newConfig
                            primaryAction:[UIAction actionWithHandler:^(UIAction*) {
                              [weakSelf newConnection];
                            }]];
    newButton.accessibilityLabel = @"New Connection";
    newItem_ = [[UIBarButtonItem alloc] initWithCustomView:newButton];
    UIBarButtonItem* settings =
        [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"gearshape"]
                                         style:UIBarButtonItemStylePlain
                                        target:self
                                        action:@selector(openSettings)];
    settings.accessibilityLabel = @"Settings";
    self.navigationItem.rightBarButtonItem = settings;

    UIButtonConfiguration* config = UIButtonConfiguration.plainButtonConfiguration;
    config.image =
        [UIImage systemImageNamed:@"chevron.down"
                withConfiguration:[UIImageSymbolConfiguration
                                      configurationWithTextStyle:UIFontTextStyleCaption1
                                                           scale:UIImageSymbolScaleSmall]];
    config.imagePlacement = NSDirectionalRectEdgeTrailing;
    config.imagePadding = 4;
    config.titleTextAttributesTransformer = ^(NSDictionary* attrs) {
      NSMutableDictionary* a = [attrs mutableCopy];
      a[NSFontAttributeName] = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
      return a;
    };
    workspaceButton_ = [UIButton buttonWithConfiguration:config primaryAction:nil];
    workspaceButton_.showsMenuAsPrimaryAction = YES;
    workspaceButton_.accessibilityHint = @"Switch, create, rename or delete workspaces";
    workspaceButton_.menu = [UIMenu
        menuWithChildren:@[ [UIDeferredMenuElement
                             elementWithUncachedProvider:^(void (^done)(NSArray<UIMenuElement*>*)) {
                               done([weakSelf workspaceMenu]);
                             }] ]];
    self.navigationItem.titleView = workspaceButton_;
    [self updateBar];
}

- (void)updateBar {
    // ponytail: same width test as Application::renderMainUI's compact layout
    const bool compact = gView.bounds.size.width < 700;
    NSArray* left = compact ? @[ newItem_ ] : @[ sidebarItem_, newItem_ ];
    if (![self.navigationItem.leftBarButtonItems isEqualToArray:left])
        self.navigationItem.leftBarButtonItems = left;

    auto& app = Application::getInstance();
    std::string name = "Workspace";
    for (const auto& ws : app.getWorkspaces())
        if (ws.id == app.getCurrentWorkspaceId())
            name = ws.name;
    UIButtonConfiguration* config = workspaceButton_.configuration;
    config.title = @(name.c_str());
    workspaceButton_.configuration = config;
    [workspaceButton_ sizeToFit];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self updateBar]; // rotation, split view, slide over
}

- (NSArray<UIMenuElement*>*)workspaceMenu {
    auto& app = Application::getInstance();
    const auto workspaces = app.getWorkspaces();
    const int current = app.getCurrentWorkspaceId();
    __weak DearSQLViewController* weakSelf = self;

    NSMutableArray* switching = [NSMutableArray array];
    NSString* currentName = @"";
    for (const auto& ws : workspaces) {
        const int wsId = ws.id;
        NSString* name = @(ws.name.c_str());
        if (wsId == current)
            currentName = name;
        UIAction* a = [UIAction actionWithTitle:name
                                          image:nil
                                     identifier:nil
                                        handler:^(UIAction*) {
                                          Application::getInstance().setCurrentWorkspace(wsId);
                                          [weakSelf updateBar];
                                        }];
        a.state = wsId == current ? UIMenuElementStateOn : UIMenuElementStateOff;
        [switching addObject:a];
    }

    NSMutableArray* manage = [NSMutableArray array];
    [manage addObject:[UIAction
                          actionWithTitle:@"New Workspace…"
                                    image:[UIImage systemImageNamed:@"plus.rectangle.on.rectangle"]
                               identifier:nil
                                  handler:^(UIAction*) {
                                    [weakSelf askName:@"New Workspace"
                                                 text:@""
                                               action:@"Create"
                                                 done:^(NSString* n) {
                                                   Application::getInstance().createWorkspace(
                                                       n.UTF8String);
                                                 }];
                                  }]];
    [manage
        addObject:[UIAction actionWithTitle:[NSString stringWithFormat:@"Rename “%@”…", currentName]
                                      image:[UIImage systemImageNamed:@"pencil"]
                                 identifier:nil
                                    handler:^(UIAction*) {
                                      [weakSelf askName:@"Rename Workspace"
                                                   text:currentName
                                                 action:@"Rename"
                                                   done:^(NSString* n) {
                                                     Application::getInstance().renameWorkspace(
                                                         current, n.UTF8String);
                                                   }];
                                    }]];
    if (current != 1) { // Default can't be deleted
        UIAction* del =
            [UIAction actionWithTitle:[NSString stringWithFormat:@"Delete “%@”", currentName]
                                image:[UIImage systemImageNamed:@"trash"]
                           identifier:nil
                              handler:^(UIAction*) {
                                [weakSelf confirmDeleteWorkspace:current name:currentName];
                              }];
        del.attributes = UIMenuElementAttributesDestructive;
        [manage addObject:del];
    }
    return @[
        [UIMenu menuWithTitle:@""
                        image:nil
                   identifier:nil
                      options:UIMenuOptionsDisplayInline
                     children:switching],
        [UIMenu menuWithTitle:@""
                        image:nil
                   identifier:nil
                      options:UIMenuOptionsDisplayInline
                     children:manage],
    ];
}

- (void)askName:(NSString*)title
           text:(NSString*)text
         action:(NSString*)action
           done:(void (^)(NSString*))done {
    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:title
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
      field.text = text;
      field.placeholder = @"Workspace name";
      field.autocapitalizationType = UITextAutocapitalizationTypeWords;
      field.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    __weak UIAlertController* weakAlert = alert;
    __weak DearSQLViewController* weakSelf = self;
    UIAlertAction* ok = [UIAlertAction
        actionWithTitle:action
                  style:UIAlertActionStyleDefault
                handler:^(UIAlertAction*) {
                  NSString* name = [weakAlert.textFields.firstObject.text
                      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                  if (name.length > 0) {
                      done(name);
                      [weakSelf updateBar];
                  }
                }];
    [alert addAction:ok];
    alert.preferredAction = ok;
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)confirmDeleteWorkspace:(int)wsId name:(NSString*)name {
    UIAlertController* alert = [UIAlertController
        alertControllerWithTitle:[NSString stringWithFormat:@"Delete “%@”?", name]
                         message:@"Its connections move to the Default workspace."
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    __weak DearSQLViewController* weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete"
                                              style:UIAlertActionStyleDestructive
                                            handler:^(UIAlertAction*) {
                                              Application::getInstance().deleteWorkspace(wsId);
                                              [weakSelf updateBar];
                                            }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)toggleSidebar {
    auto& app = Application::getInstance();
    app.setSidebarVisible(!app.isSidebarVisible());
}
- (void)newConnection {
    ConnectionDialog::instance().show(&Application::getInstance());
}
- (void)openSettings {
    SettingsDialog::instance().open();
}

- (NSArray<UIKeyCommand*>*)keyCommands {
    UIKeyCommand* add = [UIKeyCommand commandWithTitle:@"New Connection"
                                                 image:nil
                                                action:@selector(newConnection)
                                                 input:@"n"
                                         modifierFlags:UIKeyModifierCommand
                                          propertyList:nil];
    UIKeyCommand* settings = [UIKeyCommand commandWithTitle:@"Settings"
                                                      image:nil
                                                     action:@selector(openSettings)
                                                      input:@","
                                              modifierFlags:UIKeyModifierCommand
                                               propertyList:nil];
    UIKeyCommand* sidebar =
        [UIKeyCommand commandWithTitle:@"Toggle Sidebar"
                                 image:nil
                                action:@selector(toggleSidebar)
                                 input:@"s"
                         modifierFlags:UIKeyModifierCommand | UIKeyModifierControl
                          propertyList:nil];
    for (UIKeyCommand* c in @[ add, settings, sidebar ])
        c.wantsPriorityOverSystemBehavior = YES;
    return @[ add, settings, sidebar ];
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
    editMenu_ = [[UIEditMenuInteraction alloc] initWithDelegate:self];
    [view addInteraction:editMenu_];
    // the imgui view keeps clear of the notch in landscape
    UIView* container = [[UIView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [container addSubview:view];
    [NSLayoutConstraint activateConstraints:@[
        [view.topAnchor constraintEqualToAnchor:container.topAnchor],
        [view.bottomAnchor constraintEqualToAnchor:container.bottomAnchor],
        [view.leadingAnchor constraintEqualToAnchor:container.safeAreaLayoutGuide.leadingAnchor],
        [view.trailingAnchor constraintEqualToAnchor:container.safeAreaLayoutGuide.trailingAnchor],
    ]];
    self.view = container;
    gView = view;
    view.tag = kDearSQLViewTag;
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
    [gView becomeFirstResponder];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.edgesForExtendedLayout = UIRectEdgeNone; // imgui y = 0 is below the bar, for dialogs too
    [self setupBar];
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
    const CGRect local = [gView convertRect:screen
                        fromCoordinateSpace:gView.window.screen.coordinateSpace];
    const CGRect covered = CGRectIntersection(gView.bounds, local);
    gKeyboardInset = CGRectIsNull(covered) || CGRectGetMaxY(covered) < CGRectGetMaxY(gView.bounds)
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
    touchAt([g locationInView:gView]);
    return NO;
}
- (void)hover:(UIHoverGestureRecognizer*)g {
    const CGPoint p = [g locationInView:gView];
    ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    ImGui::GetIO().AddMousePosEvent(p.x, p.y);
}
- (void)pan:(UIPanGestureRecognizer*)g {
    const CGPoint d = [g translationInView:gView];
    [g setTranslation:CGPointZero inView:gView];
    if (g.numberOfTouches > 0)
        touchAt([g locationInView:gView]); // scroll the window under the finger
    // a wheel step scrolls 2 font sizes sideways, 5 down: the content stays under the finger
    const float font = std::max(ImGui::GetFontSize(), 1.0f);
    ImGui::GetIO().AddMouseWheelEvent(d.x / (2.0f * font), d.y / (5.0f * font));
}
- (void)longPress:(UILongPressGestureRecognizer*)g {
    if (g.state != UIGestureRecognizerStateBegan)
        return;
    [[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight] impactOccurred];
    ImGuiIO& io = ImGui::GetIO();
    if (gTouch == Touch::Pressed)
        releaseAway();
    gTouch = Touch::None;
    const CGPoint p = [g locationInView:gView];
    touchAt(p);
    // a few frames for imgui to report what is under the finger: text gets the edit menu,
    // anything else a right click
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
          ImGuiIO& io = ImGui::GetIO();
          if (ImGui::GetMouseCursor() != ImGuiMouseCursor_TextInput) {
              io.AddMouseButtonEvent(1, true);
              io.AddMouseButtonEvent(1, false);
              return;
          }
          if (!io.WantTextInput) { // focus the field first
              io.AddMouseButtonEvent(0, true);
              io.AddMouseButtonEvent(0, false);
          }
          [editMenu_ presentEditMenuWithConfiguration:[UIEditMenuConfiguration
                                                          configurationWithIdentifier:nil
                                                                          sourcePoint:p]];
        });
}
- (UIMenu*)editMenuInteraction:(UIEditMenuInteraction*)interaction
          menuForConfiguration:(UIEditMenuConfiguration*)configuration
              suggestedActions:(NSArray<UIMenuElement*>*)suggested {
    auto item = [](NSString* title, ImGuiKey key) {
        return [UIAction actionWithTitle:title
                                   image:nil
                              identifier:nil
                                 handler:^(UIAction*) {
                                   ImGuiIO& io = ImGui::GetIO();
                                   io.AddKeyEvent(ImGuiMod_Super, true);
                                   tapKey(key);
                                   io.AddKeyEvent(ImGuiMod_Super, false);
                                 }];
    };
    NSMutableArray* items =
        [NSMutableArray arrayWithArray:@[ item(@"Cut", ImGuiKey_X), item(@"Copy", ImGuiKey_C) ]];
    if (UIPasteboard.generalPasteboard.hasStrings)
        [items addObject:item(@"Paste", ImGuiKey_V)];
    [items addObject:item(@"Select All", ImGuiKey_A)];
    return [UIMenu menuWithChildren:items];
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
    gNav = [[UINavigationController alloc]
        initWithRootViewController:[[DearSQLViewController alloc] init]];
    self.window.rootViewController = gNav;
    [self.window makeKeyAndVisible];
    // ponytail: screenshots in landscape, as simctl cannot rotate a simulator
    if (getenv("DEARSQL_LANDSCAPE"))
        [(UIWindowScene*)scene
            requestGeometryUpdateWithPreferences:
                [[UIWindowSceneGeometryPreferencesIOS alloc]
                    initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscapeRight]
                                    errorHandler:nil];
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

// ponytail: for app store screenshots, as nothing can tap the simulator here.
// DEARSQL_SHOW="table:orders|sql:SELECT ..." opens those tabs on the first file database
// once its tables are loaded
static void showRequestedTabs() {
    static std::string show = getenv("DEARSQL_SHOW") ?: "";
    if (show.empty())
        return;
    auto& app = Application::getInstance();
    for (const auto& db : app.getDatabases()) {
        auto* file = dynamic_cast<FileDatabase*>(db.get());
        if (!file)
            continue;
        file->checkLoadingStatus();
        if (!file->areTablesLoaded()) {
            if (!file->isLoadingTables())
                file->startTablesLoadAsync();
            return;
        }
        std::string rest = show + "|";
        for (size_t at; (at = rest.find('|')) != std::string::npos; rest.erase(0, at + 1)) {
            const std::string part = rest.substr(0, at);
            if (part.starts_with("table:")) {
                for (const auto& table : file->getTables())
                    if (table.name == part.substr(6))
                        app.getTabManager()->createTableViewerTab(file, table);
            } else if (part.starts_with("sql:")) {
                if (auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
                        app.getTabManager()->createSQLEditorTab("Query", file))) {
                    tab->setQuery(part.substr(4));
                    tab->run();
                }
            } else if (part == "dialog:connection") {
                ConnectionDialog::instance().show(&app);
            } else if (part == "dialog:settings") {
                SettingsDialog::instance().open();
            }
        }
        show.clear();
        return;
    }
}

void IOSPlatform::renderFrame() {
    @autoreleasepool {
        ImGuiIO& io = ImGui::GetIO();
        const CGSize size = gView.bounds.size;
        const CGFloat scale = gView.contentScaleFactor;
        io.DisplaySize =
            ImVec2(size.width,
                   std::max<CGFloat>(
                       size.height - std::max(gKeyboardInset, gView.safeAreaInsets.bottom), 1));
        io.DisplayFramebufferScale = ImVec2(scale, scale);
        const CFTimeInterval now = CACurrentMediaTime();
        io.DeltaTime = gLastTime > 0 ? static_cast<float>(now - gLastTime) : 1.0f / 60.0f;
        gLastTime = now;

        MTLRenderPassDescriptor* pass = gView.currentRenderPassDescriptor;
        if (!pass)
            return;
        const ImVec4 base = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        pass.colorAttachments[0].clearColor = MTLClearColorMake(base.x, base.y, base.z, 1);
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;

        id<MTLCommandBuffer> commands = [gQueue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        // ponytail: sideways only, vertical padding would mis-pick adjacent rows
        ImGui::GetStyle().TouchExtraPadding = ImVec2(Theme::Spacing::S, 0.0f);
        ImGui_ImplMetal_NewFrame(pass);
        ImGui::NewFrame();
        app_->renderMainUI();
        showRequestedTabs();
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

void IOSPlatform::applyTheme(const bool dark) {
    if (!gNav)
        return;
    const auto& c = app_->getCurrentColors();
    auto color = [](const ImVec4& v) {
        return [UIColor colorWithRed:v.x green:v.y blue:v.z alpha:v.w];
    };
    UINavigationBarAppearance* look = [[UINavigationBarAppearance alloc] init];
    [look configureWithOpaqueBackground];
    look.backgroundColor = color(c.mantle);
    look.shadowColor = color(c.surface0);
    look.titleTextAttributes = @{NSForegroundColorAttributeName : color(c.text)};
    UINavigationBar* bar = gNav.navigationBar;
    bar.standardAppearance = look;
    bar.compactAppearance = look;
    bar.scrollEdgeAppearance = look;
    bar.compactScrollEdgeAppearance = look;
    bar.tintColor = color(c.text);
    gNav.view.backgroundColor = color(c.base); // beside the notch in landscape
    gNav.overrideUserInterfaceStyle = dark ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
}
