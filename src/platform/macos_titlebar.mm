#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "mydear/title_bar.hpp"
#include "platform/alert.hpp"
#include "platform/titlebar.hpp"
#include "themes.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/title_bar.hpp"
#import <GLFW/glfw3.h>
#import <GLFW/glfw3native.h>

static void attachDialogToMainWindow(NSWindow* dialog, NSWindow* mainWindow) {
    if (!dialog || !mainWindow) {
        return;
    }

    [dialog setLevel:NSNormalWindowLevel];
    [dialog setHidesOnDeactivate:YES];

    NSRect mainFrame = mainWindow.frame;
    NSRect dialogFrame = dialog.frame;
    CGFloat x = NSMidX(mainFrame) - dialogFrame.size.width / 2;
    CGFloat y = NSMidY(mainFrame) - dialogFrame.size.height / 2;
    [dialog setFrameOrigin:NSMakePoint(x, y)];

    if (dialog.parentWindow != mainWindow) {
        [dialog.parentWindow removeChildWindow:dialog];
        [mainWindow addChildWindow:dialog ordered:NSWindowAbove];
    }
}

// license dialog, bug report and the edit menu: the settings dialog calls these
@interface MacOSTitlebarDelegate : NSObject
@property(nonatomic, assign) Application* app;
- (void)showLicenseDialog;
- (void)reportBugClicked:(id)sender;
@end

@implementation MacOSTitlebarDelegate
- (void)ensureEditMenu {
    NSMenu* mainMenu = [NSApp mainMenu];
    if (!mainMenu) {
        mainMenu = [[NSMenu alloc] init];
        [NSApp setMainMenu:mainMenu];
    }

    NSMenuItem* editMenuItem = nil;
    for (NSMenuItem* item in mainMenu.itemArray) {
        if ([item.title isEqualToString:@"Edit"]) {
            editMenuItem = item;
            break;
        }
    }

    if (!editMenuItem) {
        editMenuItem = [[NSMenuItem alloc] init];
        editMenuItem.title = @"Edit";
        NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];

        NSMenuItem* undoItem = [[NSMenuItem alloc] initWithTitle:@"Undo"
                                                          action:@selector(undo:)
                                                   keyEquivalent:@"z"];
        [editMenu addItem:undoItem];

        NSMenuItem* redoItem = [[NSMenuItem alloc] initWithTitle:@"Redo"
                                                          action:@selector(redo:)
                                                   keyEquivalent:@"Z"];
        [editMenu addItem:redoItem];

        [editMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* cutItem = [[NSMenuItem alloc] initWithTitle:@"Cut"
                                                         action:@selector(cut:)
                                                  keyEquivalent:@"x"];
        [editMenu addItem:cutItem];

        NSMenuItem* copyItem = [[NSMenuItem alloc] initWithTitle:@"Copy"
                                                          action:@selector(copy:)
                                                   keyEquivalent:@"c"];
        [editMenu addItem:copyItem];

        NSMenuItem* pasteItem = [[NSMenuItem alloc] initWithTitle:@"Paste"
                                                           action:@selector(paste:)
                                                    keyEquivalent:@"v"];
        [editMenu addItem:pasteItem];

        NSMenuItem* selectAllItem = [[NSMenuItem alloc] initWithTitle:@"Select All"
                                                               action:@selector(selectAll:)
                                                        keyEquivalent:@"a"];
        [editMenu addItem:selectAllItem];

        editMenuItem.submenu = editMenu;
        [mainMenu addItem:editMenuItem];
    }
}

- (void)showLicenseDialog {
    [self ensureEditMenu];

    auto& licenseManager = LicenseManager::instance();

    NSWindow* mainWindow = nil;
    if (self.app) {
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            mainWindow = glfwGetCocoaWindow(glfwWindow);
        }
    }

    std::string machineId = licenseManager.getInstanceId();

    NSWindow* dialog =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 450, 230)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    [dialog setTitle:@"Manage License"];
    [dialog center];

    NSView* contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 450, 230)];

    if (licenseManager.hasValidLicense()) {
        const auto info = licenseManager.getLicenseInfo();

        std::string maskedKey = info.licenseKey;
        if (maskedKey.length() > 8) {
            maskedKey = maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
        }

        NSImageView* statusIcon = [[NSImageView alloc] initWithFrame:NSMakeRect(24, 180, 20, 20)];
        statusIcon.image = [NSImage imageWithSystemSymbolName:@"checkmark.circle.fill"
                                     accessibilityDescription:@"Active"];
        statusIcon.contentTintColor = [NSColor systemGreenColor];
        [contentView addSubview:statusIcon];

        NSTextField* statusLabel = [NSTextField labelWithString:@"License Active"];
        statusLabel.frame = NSMakeRect(48, 178, 200, 24);
        statusLabel.font = [NSFont boldSystemFontOfSize:16];
        [contentView addSubview:statusLabel];

        NSTextField* emailLabel = [NSTextField labelWithString:@"Email:"];
        emailLabel.frame = NSMakeRect(24, 145, 80, 20);
        emailLabel.textColor = [NSColor secondaryLabelColor];
        emailLabel.alignment = NSTextAlignmentRight;
        [contentView addSubview:emailLabel];

        NSString* emailValue = info.customerEmail.empty()
                                   ? @"N/A"
                                   : [NSString stringWithUTF8String:info.customerEmail.c_str()];
        NSTextField* emailValueLabel = [NSTextField labelWithString:emailValue];
        emailValueLabel.frame = NSMakeRect(112, 145, 310, 20);
        emailValueLabel.selectable = YES;
        [contentView addSubview:emailValueLabel];

        NSTextField* keyLabel = [NSTextField labelWithString:@"Key:"];
        keyLabel.frame = NSMakeRect(24, 120, 80, 20);
        keyLabel.textColor = [NSColor secondaryLabelColor];
        keyLabel.alignment = NSTextAlignmentRight;
        [contentView addSubview:keyLabel];

        NSTextField* keyValueLabel =
            [NSTextField labelWithString:[NSString stringWithUTF8String:maskedKey.c_str()]];
        keyValueLabel.frame = NSMakeRect(112, 120, 310, 20);
        [contentView addSubview:keyValueLabel];

        NSTextField* machineLabel = [NSTextField labelWithString:@"Device ID:"];
        machineLabel.frame = NSMakeRect(24, 95, 80, 20);
        machineLabel.textColor = [NSColor secondaryLabelColor];
        machineLabel.alignment = NSTextAlignmentRight;
        [contentView addSubview:machineLabel];

        NSTextField* machineValueLabel =
            [NSTextField labelWithString:[NSString stringWithUTF8String:machineId.c_str()]];
        machineValueLabel.frame = NSMakeRect(112, 95, 310, 20);
        machineValueLabel.selectable = YES;
        machineValueLabel.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        [contentView addSubview:machineValueLabel];

        NSTextField* statusMessageLabel = [NSTextField labelWithString:@""];
        statusMessageLabel.frame = NSMakeRect(24, 65, 400, 20);
        statusMessageLabel.textColor = [NSColor systemRedColor];
        [contentView addSubview:statusMessageLabel];

        NSButton* closeButton = [[NSButton alloc] initWithFrame:NSMakeRect(340, 16, 90, 32)];
        closeButton.title = @"Close";
        closeButton.bezelStyle = NSBezelStyleRounded;
        closeButton.target = dialog;
        closeButton.action = @selector(close);
        [contentView addSubview:closeButton];

        NSButton* deactivateButton = [[NSButton alloc] initWithFrame:NSMakeRect(230, 16, 100, 32)];
        deactivateButton.title = @"Deactivate";
        deactivateButton.bezelStyle = NSBezelStyleRounded;
        deactivateButton.hasDestructiveAction = YES;
        [contentView addSubview:deactivateButton];

        objc_setAssociatedObject(deactivateButton, "dialog", dialog, OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(deactivateButton, "statusLabel", statusMessageLabel,
                                 OBJC_ASSOCIATION_RETAIN);

        [deactivateButton setTarget:self];
        [deactivateButton setAction:@selector(deactivateLicenseFromDialog:)];

    } else {
        NSTextField* titleLabel = [NSTextField labelWithString:@"Register License"];
        titleLabel.frame = NSMakeRect(24, 185, 400, 24);
        titleLabel.font = [NSFont boldSystemFontOfSize:16];
        [contentView addSubview:titleLabel];

        NSTextField* descLabel =
            [NSTextField labelWithString:@"Enter your license key to activate DearSQL:"];
        descLabel.frame = NSMakeRect(24, 160, 400, 20);
        [contentView addSubview:descLabel];

        NSTextField* keyField = [[NSTextField alloc] initWithFrame:NSMakeRect(24, 125, 400, 28)];
        keyField.placeholderString = @"XXXX-XXXX-XXXX-XXXX";
        keyField.editable = YES;
        keyField.selectable = YES;
        keyField.bezeled = YES;
        keyField.bezelStyle = NSTextFieldRoundedBezel;
        [contentView addSubview:keyField];

        objc_setAssociatedObject(dialog, "keyField", keyField, OBJC_ASSOCIATION_RETAIN);

        NSTextField* machineLabel = [NSTextField labelWithString:@"Device ID:"];
        machineLabel.frame = NSMakeRect(24, 95, 80, 20);
        machineLabel.textColor = [NSColor secondaryLabelColor];
        [contentView addSubview:machineLabel];

        NSTextField* machineValueLabel =
            [NSTextField labelWithString:[NSString stringWithUTF8String:machineId.c_str()]];
        machineValueLabel.frame = NSMakeRect(105, 95, 320, 20);
        machineValueLabel.selectable = YES;
        machineValueLabel.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        [contentView addSubview:machineValueLabel];

        NSTextField* statusMessageLabel = [NSTextField labelWithString:@""];
        statusMessageLabel.frame = NSMakeRect(24, 70, 400, 20);
        statusMessageLabel.textColor = [NSColor systemRedColor];
        [contentView addSubview:statusMessageLabel];

        NSTextField* linkText = [NSTextField labelWithString:@"Don't have a license?"];
        linkText.frame = NSMakeRect(24, 45, 130, 20);
        linkText.textColor = [NSColor secondaryLabelColor];
        [contentView addSubview:linkText];

        NSButton* purchaseLink = [[NSButton alloc] initWithFrame:NSMakeRect(154, 43, 100, 24)];
        purchaseLink.title = @"Purchase one";
        purchaseLink.bezelStyle = NSBezelStyleInline;
        purchaseLink.bordered = NO;
        NSMutableAttributedString* attrTitle = [[NSMutableAttributedString alloc]
            initWithString:@"Purchase one"
                attributes:@{
                    NSForegroundColorAttributeName : [NSColor linkColor],
                    NSUnderlineStyleAttributeName : @(NSUnderlineStyleSingle)
                }];
        purchaseLink.attributedTitle = attrTitle;
        purchaseLink.target = self;
        purchaseLink.action = @selector(openPurchaseLink:);
        [contentView addSubview:purchaseLink];

        NSButton* cancelButton = [[NSButton alloc] initWithFrame:NSMakeRect(250, 16, 90, 32)];
        cancelButton.title = @"Cancel";
        cancelButton.bezelStyle = NSBezelStyleRounded;
        cancelButton.target = dialog;
        cancelButton.action = @selector(close);
        [contentView addSubview:cancelButton];

        NSButton* activateButton = [[NSButton alloc] initWithFrame:NSMakeRect(345, 16, 90, 32)];
        activateButton.title = @"Activate";
        activateButton.bezelStyle = NSBezelStyleRounded;
        activateButton.keyEquivalent = @"\r";
        [contentView addSubview:activateButton];

        objc_setAssociatedObject(activateButton, "dialog", dialog, OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(activateButton, "keyField", keyField, OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(activateButton, "statusLabel", statusMessageLabel,
                                 OBJC_ASSOCIATION_RETAIN);

        activateButton.target = self;
        activateButton.action = @selector(activateLicenseFromDialog:);
    }

    dialog.contentView = contentView;

    NSTextField* keyFieldToFocus = objc_getAssociatedObject(dialog, "keyField");

    if (keyFieldToFocus) {
        dialog.initialFirstResponder = keyFieldToFocus;
    }

    attachDialogToMainWindow(dialog, mainWindow);

    [dialog makeKeyAndOrderFront:nil];
    if (keyFieldToFocus) {
        [dialog makeFirstResponder:keyFieldToFocus];
    }
}

- (void)reportBugClicked:(id)sender {
    NSString* version = @APP_VERSION;
    NSString* urlStr = [NSString
        stringWithFormat:@"https://github.com/dunkbing/dearsql/issues/new?labels=bug"
                          "&title=%%5BBug%%5D+&body=%%23%%23+Description%%0A%%0A%%23%%23+Steps+"
                          "to+Reproduce%%0A1.+%%0A2.+%%0A3.+%%0A%%0A%%23%%23+Expected+Behavior"
                          "%%0A%%0A%%23%%23+Actual+Behavior%%0A%%0A%%23%%23+Environment%%0A-+**OS"
                          "**%%3A+macOS%%0A-+**DearSQL+version**%%3A+%@%%0A-+**Database**%%3A+",
                         version];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:urlStr]];
}

- (void)openPurchaseLink:(id)sender {
    NSURL* url = [NSURL URLWithString:@PURCHASE_URL];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)activateLicenseFromDialog:(NSButton*)sender {
    NSWindow* dialog = objc_getAssociatedObject(sender, "dialog");
    NSTextField* keyField = objc_getAssociatedObject(sender, "keyField");
    NSTextField* statusLabel = objc_getAssociatedObject(sender, "statusLabel");

    NSString* key = keyField.stringValue;
    if (key.length == 0) {
        statusLabel.stringValue = @"Please enter a license key";
        return;
    }

    statusLabel.stringValue = @"Activating...";
    statusLabel.textColor = [NSColor secondaryLabelColor];
    sender.enabled = NO;

    std::string licenseKey = [key UTF8String];

    // retain objects for async operation (MRC)
    [dialog retain];
    [sender retain];
    [statusLabel retain];

    LicenseManager::instance().activateLicense(
        licenseKey, [dialog, sender, statusLabel](const LicenseInfo& result) {
            NSLog(@"License activation callback: valid=%d, status=%s, error=%s", result.valid,
                  result.status.c_str(), result.error.c_str());

            bool isValid = result.valid;
            std::string errorMsg = result.error;

            dispatch_async(dispatch_get_main_queue(), ^{
              NSLog(@"License activation main thread: isValid=%d", isValid);
              if (isValid) {
                  NSLog(@"License activation: closing dialog");
                  [dialog close];
              } else {
                  std::string err = errorMsg.empty() ? "Activation failed" : errorMsg;
                  NSLog(@"License activation: showing error - %s", err.c_str());
                  statusLabel.stringValue = [NSString stringWithUTF8String:err.c_str()];
                  statusLabel.textColor = [NSColor systemRedColor];
                  sender.enabled = YES;
              }
              [dialog release];
              [sender release];
              [statusLabel release];
            });
        });
}

- (void)deactivateLicenseFromDialog:(NSButton*)sender {
    NSWindow* dialog = objc_getAssociatedObject(sender, "dialog");
    NSTextField* statusLabel = objc_getAssociatedObject(sender, "statusLabel");

    statusLabel.stringValue = @"Deactivating...";
    statusLabel.textColor = [NSColor secondaryLabelColor];
    sender.enabled = NO;

    // retain objects for async operation (MRC)
    [dialog retain];
    [sender retain];
    [statusLabel retain];

    LicenseManager::instance().deactivateLicense(
        [dialog, sender, statusLabel](const LicenseInfo& result) {
            std::string errorMsg = result.error;

            dispatch_async(dispatch_get_main_queue(), ^{
              if (errorMsg.empty()) {
                  [dialog close];
              } else {
                  statusLabel.stringValue = [NSString stringWithUTF8String:errorMsg.c_str()];
                  statusLabel.textColor = [NSColor systemRedColor];
                  sender.enabled = YES;
              }
              [dialog release];
              [sender release];
              [statusLabel release];
            });
        });
}

@end

// ---- MacOSTitlebar: the shared imgui bar in mydear's title bar strip ----

MacOSTitlebar::MacOSTitlebar(Application* app, GLFWwindow* window)
    : app_(app), window_(window), delegate_([[MacOSTitlebarDelegate alloc] init]) {
    delegate_.app = app_;
}

MacOSTitlebar::~MacOSTitlebar() {
    delegate_ = nullptr;
}

void MacOSTitlebar::setup() {
    // native-only actions the shared settings dialog can't do itself
    MacOSTitlebarDelegate* d = delegate_;
    SettingsDialog::instance().onManageLicense = [d]() { [d showLicenseDialog]; };
    SettingsDialog::instance().onReportBug = [d]() { [d reportBugClicked:nil]; };
    applyTheme(app_->isDarkTheme());
}

void MacOSTitlebar::render() {
    TitleBarView::render(*app_);
}

float MacOSTitlebar::getHeight() const {
    return mydear::TitleBarHeight();
}

void MacOSTitlebar::applyTheme(bool isDark) {
    NSWindow* nsWindow = window_ ? glfwGetCocoaWindow(window_) : nil;
    if (!nsWindow)
        return;
    // the traffic lights and native sheets follow the window's appearance
    const auto& c = Application::getInstance().getCurrentColors().base;
    nsWindow.backgroundColor = [NSColor colorWithRed:c.x green:c.y blue:c.z alpha:c.w];
    nsWindow.appearance =
        [NSAppearance appearanceNamed:isDark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}
