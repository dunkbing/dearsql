#include "platform/macos_platform.hpp"
#include "application.hpp"
#include "config.hpp"
#include "imgui_impl_glfw.h"
#include "license/license_manager.hpp"
#include "themes.hpp"
#include <iostream>

#include "platform/macos_updater.hpp"
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#import "imgui_impl_metal.h"
#import <GLFW/glfw3native.h>

// Pass-through views: hitTest returns nil so all events reach GLFW's content view.
@interface PassthroughView : NSView
@end
@implementation PassthroughView
- (NSView*)hitTest:(NSPoint)point {
    return nil;
}
@end

@interface PassthroughEffectView : NSVisualEffectView
@end
@implementation PassthroughEffectView
- (NSView*)hitTest:(NSPoint)point {
    return nil;
}
@end

// Toolbar delegate interface
@interface ToolbarDelegate : NSObject <NSToolbarDelegate>
@property(nonatomic, assign) Application* app;
@property(nonatomic, strong) NSPopUpButton* workspaceDropdown;
@property(nonatomic, strong) NSPopover* menuPopover;
@property(nonatomic, strong) NSButton* themeLightButton;
@property(nonatomic, strong) NSButton* themeDarkButton;
@property(nonatomic, strong) NSButton* themeAutoButton;
- (void)showMenuPopover:(NSButton*)sender;
- (void)updateThemeButtons;
- (void)updateWindowBackgroundColor;
@end

@implementation ToolbarDelegate
- (NSArray<NSToolbarItemIdentifier>*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar {
    return @[ NSToolbarFlexibleSpaceItemIdentifier, @"WorkspaceSelector", @"MenuButton" ];
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar {
    return @[ @"WorkspaceSelector", @"MenuButton", NSToolbarFlexibleSpaceItemIdentifier ];
}

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)itemIdentifier
    willBeInsertedIntoToolbar:(BOOL)flag {
    if ([itemIdentifier isEqualToString:@"WorkspaceSelector"]) {
        NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:itemIdentifier];
        item.label = @"Workspace";
        item.paletteLabel = @"Workspace Selector";
        item.toolTip = @"Select Workspace";

        self.workspaceDropdown = [[NSPopUpButton alloc] init];
        [self.workspaceDropdown setBezelStyle:NSBezelStyleTexturedRounded];
        [self.workspaceDropdown setBordered:YES];
        [self.workspaceDropdown setTarget:self];
        [self.workspaceDropdown setAction:@selector(workspaceChanged:)];
        [self updateWorkspaceDropdown];
        [self.workspaceDropdown sizeToFit];

        item.view = self.workspaceDropdown;
        return item;
    } else if ([itemIdentifier isEqualToString:@"MenuButton"]) {
        NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:itemIdentifier];
        item.label = @"Menu";
        item.paletteLabel = @"Menu";
        item.toolTip = @"Main Menu";

        NSButton* menuButton = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        [menuButton setImage:[NSImage imageWithSystemSymbolName:@"line.3.horizontal"
                                       accessibilityDescription:@"Menu"]];
        [menuButton setButtonType:NSButtonTypeMomentaryPushIn];
        [menuButton setBezelStyle:NSBezelStyleTexturedRounded];
        [menuButton setBordered:NO];
        [menuButton setTarget:self];
        [menuButton setAction:@selector(showMenuPopover:)];
        [menuButton sizeToFit];

        item.view = menuButton;
        return item;
    }
    return nil;
}

- (void)connectButtonClicked:(id)sender {
    @try {
        if (self.app) {
            // Show the connection dialog directly
            if (self.app->getDatabaseSidebar()) {
                self.app->getDatabaseSidebar()->showConnectionDialog();
            }
        }
    } @catch (NSException* exception) {
        NSLog(@"Exception in connectButtonClicked: %@", exception);
    }
}

- (void)sidebarToggleClicked:(id)sender {
    @try {
        if (self.app) {
            self.app->onSidebarToggleClicked();
        }
    } @catch (NSException* exception) {
        NSLog(@"Exception in sidebarToggleClicked: %@", exception);
    }
}

- (void)workspaceChanged:(id)sender {
    @try {
        if (self.app && self.workspaceDropdown) {
            NSInteger selectedIndex = [self.workspaceDropdown indexOfSelectedItem];
            if (selectedIndex >= 0) {
                NSMenuItem* selectedItem = [self.workspaceDropdown itemAtIndex:selectedIndex];
                int workspaceId = (int)selectedItem.tag;
                self.app->setCurrentWorkspace(workspaceId);
            }
        }
    } @catch (NSException* exception) {
        NSLog(@"Exception in workspaceChanged: %@", exception);
    }
}

- (void)updateWorkspaceDropdown {
    if (!self.workspaceDropdown || !self.app) {
        return;
    }

    [self.workspaceDropdown removeAllItems];

    auto workspaces = self.app->getWorkspaces();
    int currentWorkspaceId = self.app->getCurrentWorkspaceId();

    for (const auto& workspace : workspaces) {
        NSString* title = [NSString stringWithUTF8String:workspace.name.c_str()];
        [self.workspaceDropdown addItemWithTitle:title];

        NSMenuItem* item = [self.workspaceDropdown lastItem];
        item.tag = workspace.id;

        if (workspace.id == currentWorkspaceId) {
            [self.workspaceDropdown selectItem:item];
        }
    }

    // Add "New Workspace..." option
    [self.workspaceDropdown.menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* newWorkspaceItem = [[NSMenuItem alloc] initWithTitle:@"New Workspace..."
                                                              action:@selector(createNewWorkspace:)
                                                       keyEquivalent:@""];
    newWorkspaceItem.target = self;
    [self.workspaceDropdown.menu addItem:newWorkspaceItem];
}

- (void)createNewWorkspace:(id)sender {
    @try {
        if (!self.app)
            return;

        // Get the main window from the application
        NSWindow* mainWindow = nil;
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            mainWindow = glfwGetCocoaWindow(glfwWindow);
        }

        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Create New Workspace";
        alert.informativeText = @"Enter a name for the new workspace:";
        [alert addButtonWithTitle:@"Create"];
        [alert addButtonWithTitle:@"Cancel"];

        NSTextField* textField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
        textField.placeholderString = @"Workspace name";
        alert.accessoryView = textField;

        if (mainWindow) {
            [alert beginSheetModalForWindow:mainWindow
                          completionHandler:^(NSModalResponse returnCode) {
                            if (returnCode == NSAlertFirstButtonReturn) {
                                NSString* workspaceName = textField.stringValue;
                                if (workspaceName.length > 0 && self.app) {
                                    std::string name = [workspaceName UTF8String];
                                    int newWorkspaceId = self.app->createWorkspace(name);
                                    if (newWorkspaceId > 0) {
                                        [self updateWorkspaceDropdown];
                                    }
                                }
                            }
                          }];
        } else {
            // Fallback to modal dialog if no main window
            NSModalResponse returnCode = [alert runModal];
            if (returnCode == NSAlertFirstButtonReturn) {
                NSString* workspaceName = textField.stringValue;
                if (workspaceName.length > 0 && self.app) {
                    std::string name = [workspaceName UTF8String];
                    int newWorkspaceId = self.app->createWorkspace(name);
                    if (newWorkspaceId > 0) {
                        [self updateWorkspaceDropdown];
                    }
                }
            }
        }
    } @catch (NSException* exception) {
        NSLog(@"Exception in createNewWorkspace: %@", exception);
    }
}

- (void)showMenuPopover:(NSButton*)sender {
    @try {
        if (!self.menuPopover) {
            [self createMenuPopover];
        }
        [self updateThemeButtons];
        [self.menuPopover showRelativeToRect:sender.bounds
                                      ofView:sender
                               preferredEdge:NSRectEdgeMaxY];
    } @catch (NSException* exception) {
        NSLog(@"Exception in showMenuPopover: %@", exception);
    }
}

- (void)createMenuPopover {
    self.menuPopover = [[NSPopover alloc] init];
    self.menuPopover.behavior = NSPopoverBehaviorTransient;

    // Create content view controller
    NSViewController* contentVC = [[NSViewController alloc] init];

    CGFloat extraHeight = 36;
    NSView* contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 180, 166 + extraHeight)];

    // Theme section label
    NSTextField* themeLabel = [NSTextField labelWithString:@"Theme"];
    themeLabel.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    themeLabel.textColor = [NSColor secondaryLabelColor];
    themeLabel.frame = NSMakeRect(12, 131 + extraHeight, 156, 16);
    [contentView addSubview:themeLabel];

    // Theme buttons container
    CGFloat buttonWidth = 50;
    CGFloat buttonHeight = 28;
    CGFloat buttonY = 96 + extraHeight;
    CGFloat startX = 12;
    CGFloat spacing = 4;

    // Light theme button
    self.themeLightButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(startX, buttonY, buttonWidth, buttonHeight)];
    [self.themeLightButton setImage:[NSImage imageWithSystemSymbolName:@"sun.max"
                                              accessibilityDescription:@"Light Theme"]];
    [self.themeLightButton setButtonType:NSButtonTypeMomentaryPushIn];
    [self.themeLightButton setBezelStyle:NSBezelStyleTexturedRounded];
    [self.themeLightButton setTarget:self];
    [self.themeLightButton setAction:@selector(themeLightClicked:)];
    [self.themeLightButton setToolTip:@"Light"];
    [contentView addSubview:self.themeLightButton];

    // Dark theme button
    self.themeDarkButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(startX + buttonWidth + spacing, buttonY,
                                                   buttonWidth, buttonHeight)];
    [self.themeDarkButton setImage:[NSImage imageWithSystemSymbolName:@"moon"
                                             accessibilityDescription:@"Dark Theme"]];
    [self.themeDarkButton setButtonType:NSButtonTypeMomentaryPushIn];
    [self.themeDarkButton setBezelStyle:NSBezelStyleTexturedRounded];
    [self.themeDarkButton setTarget:self];
    [self.themeDarkButton setAction:@selector(themeDarkClicked:)];
    [self.themeDarkButton setToolTip:@"Dark"];
    [contentView addSubview:self.themeDarkButton];

    // Auto theme button
    self.themeAutoButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(startX + 2 * (buttonWidth + spacing), buttonY,
                                                   buttonWidth, buttonHeight)];
    [self.themeAutoButton setImage:[NSImage imageWithSystemSymbolName:@"circle.lefthalf.filled"
                                             accessibilityDescription:@"Auto Theme"]];
    [self.themeAutoButton setButtonType:NSButtonTypeMomentaryPushIn];
    [self.themeAutoButton setBezelStyle:NSBezelStyleTexturedRounded];
    [self.themeAutoButton setTarget:self];
    [self.themeAutoButton setAction:@selector(themeAutoClicked:)];
    [self.themeAutoButton setToolTip:@"System"];
    [contentView addSubview:self.themeAutoButton];

    // Separator line
    NSBox* separator = [[NSBox alloc] initWithFrame:NSMakeRect(12, 84 + extraHeight, 156, 1)];
    separator.boxType = NSBoxSeparator;
    [contentView addSubview:separator];

    // License button
    NSButton* licenseButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(12, 48 + extraHeight, 156, 28)];
    [licenseButton setTitle:@"Manage License..."];
    [licenseButton setButtonType:NSButtonTypeMomentaryPushIn];
    [licenseButton setBezelStyle:NSBezelStyleTexturedRounded];
    [licenseButton setTarget:self];
    [licenseButton setAction:@selector(licenseClicked:)];
    [contentView addSubview:licenseButton];

    // Check for Updates button
    NSButton* updateButton = [[NSButton alloc] initWithFrame:NSMakeRect(12, 48, 156, 28)];
    [updateButton setTitle:@"Check for Updates..."];
    [updateButton setButtonType:NSButtonTypeMomentaryPushIn];
    [updateButton setBezelStyle:NSBezelStyleTexturedRounded];
    [updateButton setTarget:self];
    [updateButton setAction:@selector(checkForUpdatesClicked:)];
    [contentView addSubview:updateButton];

    // Report Bug button
    NSButton* reportBugButton = [[NSButton alloc] initWithFrame:NSMakeRect(12, 12, 156, 28)];
    [reportBugButton setTitle:@"Report Bug..."];
    [reportBugButton setButtonType:NSButtonTypeMomentaryPushIn];
    [reportBugButton setBezelStyle:NSBezelStyleTexturedRounded];
    [reportBugButton setTarget:self];
    [reportBugButton setAction:@selector(reportBugClicked:)];
    [contentView addSubview:reportBugButton];

    contentVC.view = contentView;
    self.menuPopover.contentViewController = contentVC;
}

- (void)updateThemeButtons {
    if (!self.app || !self.themeLightButton || !self.themeDarkButton || !self.themeAutoButton) {
        return;
    }

    bool isDark = self.app->isDarkTheme();

    // Setup layers for all buttons
    self.themeLightButton.wantsLayer = YES;
    self.themeDarkButton.wantsLayer = YES;
    self.themeAutoButton.wantsLayer = YES;

    // Reset all buttons to default appearance
    [self.themeLightButton setContentTintColor:nil];
    [self.themeDarkButton setContentTintColor:nil];
    [self.themeAutoButton setContentTintColor:nil];
    self.themeLightButton.layer.backgroundColor = nil;
    self.themeDarkButton.layer.backgroundColor = nil;
    self.themeAutoButton.layer.backgroundColor = nil;

    // Highlight the currently selected theme with accent color background
    NSButton* selectedButton = isDark ? self.themeDarkButton : self.themeLightButton;
    [selectedButton setContentTintColor:[NSColor controlAccentColor]];
    selectedButton.layer.backgroundColor =
        [[NSColor controlAccentColor] colorWithAlphaComponent:0.15].CGColor;
    selectedButton.layer.cornerRadius = 6;
    selectedButton.layer.masksToBounds = YES;
}

- (void)updateWindowBackgroundColor {
    if (!self.app)
        return;

    GLFWwindow* glfwWindow = self.app->getWindow();
    if (!glfwWindow)
        return;

    NSWindow* nsWindow = glfwGetCocoaWindow(glfwWindow);
    if (!nsWindow)
        return;

    const auto& colors = self.app->isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    NSColor* bgColor = [NSColor colorWithRed:colors.base.x
                                       green:colors.base.y
                                        blue:colors.base.z
                                       alpha:colors.base.w];
    [nsWindow setBackgroundColor:bgColor];

    // Update window appearance so native titlebar controls match the theme
    NSAppearanceName appearanceName =
        self.app->isDarkTheme() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    nsWindow.appearance = [NSAppearance appearanceNamed:appearanceName];
}

- (void)themeLightClicked:(id)sender {
    @try {
        if (self.app) {
            self.app->setDarkTheme(false);
        }
        [self updateThemeButtons];
        [self updateWindowBackgroundColor];
        [self.menuPopover close];
    } @catch (NSException* exception) {
        NSLog(@"Exception in themeLightClicked: %@", exception);
    }
}

- (void)themeDarkClicked:(id)sender {
    @try {
        if (self.app) {
            self.app->setDarkTheme(true);
        }
        [self updateThemeButtons];
        [self updateWindowBackgroundColor];
        [self.menuPopover close];
    } @catch (NSException* exception) {
        NSLog(@"Exception in themeDarkClicked: %@", exception);
    }
}

- (void)themeAutoClicked:(id)sender {
    @try {
        // Detect system appearance and set accordingly
        if (self.app) {
            NSAppearance* appearance = [NSApp effectiveAppearance];
            NSAppearanceName appearanceName = [appearance bestMatchFromAppearancesWithNames:@[
                NSAppearanceNameAqua, NSAppearanceNameDarkAqua
            ]];
            bool systemIsDark = [appearanceName isEqualToString:NSAppearanceNameDarkAqua];
            self.app->setDarkTheme(systemIsDark);
        }
        [self updateThemeButtons];
        [self updateWindowBackgroundColor];
        [self.menuPopover close];
    } @catch (NSException* exception) {
        NSLog(@"Exception in themeAutoClicked: %@", exception);
    }
}

- (void)licenseClicked:(id)sender {
    @try {
        [self.menuPopover close];
        [self showLicenseDialog];
    } @catch (NSException* exception) {
        NSLog(@"Exception in licenseClicked: %@", exception);
    }
}

- (void)ensureEditMenu {
    // Check if Edit menu already exists
    NSMenu* mainMenu = [NSApp mainMenu];
    if (!mainMenu) {
        mainMenu = [[NSMenu alloc] init];
        [NSApp setMainMenu:mainMenu];
    }

    // Look for existing Edit menu
    NSMenuItem* editMenuItem = nil;
    for (NSMenuItem* item in mainMenu.itemArray) {
        if ([item.title isEqualToString:@"Edit"]) {
            editMenuItem = item;
            break;
        }
    }

    if (!editMenuItem) {
        // Create Edit menu
        editMenuItem = [[NSMenuItem alloc] init];
        editMenuItem.title = @"Edit";
        NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];

        // Undo
        NSMenuItem* undoItem = [[NSMenuItem alloc] initWithTitle:@"Undo"
                                                          action:@selector(undo:)
                                                   keyEquivalent:@"z"];
        [editMenu addItem:undoItem];

        // Redo
        NSMenuItem* redoItem = [[NSMenuItem alloc] initWithTitle:@"Redo"
                                                          action:@selector(redo:)
                                                   keyEquivalent:@"Z"];
        [editMenu addItem:redoItem];

        [editMenu addItem:[NSMenuItem separatorItem]];

        // Cut
        NSMenuItem* cutItem = [[NSMenuItem alloc] initWithTitle:@"Cut"
                                                         action:@selector(cut:)
                                                  keyEquivalent:@"x"];
        [editMenu addItem:cutItem];

        // Copy
        NSMenuItem* copyItem = [[NSMenuItem alloc] initWithTitle:@"Copy"
                                                          action:@selector(copy:)
                                                   keyEquivalent:@"c"];
        [editMenu addItem:copyItem];

        // Paste
        NSMenuItem* pasteItem = [[NSMenuItem alloc] initWithTitle:@"Paste"
                                                           action:@selector(paste:)
                                                    keyEquivalent:@"v"];
        [editMenu addItem:pasteItem];

        // Select All
        NSMenuItem* selectAllItem = [[NSMenuItem alloc] initWithTitle:@"Select All"
                                                               action:@selector(selectAll:)
                                                        keyEquivalent:@"a"];
        [editMenu addItem:selectAllItem];

        editMenuItem.submenu = editMenu;
        [mainMenu addItem:editMenuItem];
    }
}

- (void)showLicenseDialog {
    // Ensure Edit menu exists for copy/paste support
    [self ensureEditMenu];

    auto& licenseManager = LicenseManager::instance();

    // Get the main window
    NSWindow* mainWindow = nil;
    if (self.app) {
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            mainWindow = glfwGetCocoaWindow(glfwWindow);
        }
    }

    // Get machine ID for display
    std::string machineId = licenseManager.getInstanceId();

    // Create dialog window
    NSWindow* dialog =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 450, 230)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    [dialog setTitle:@"Manage License"];
    [dialog center];

    NSView* contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 450, 230)];

    if (licenseManager.hasValidLicense()) {
        // Licensed view
        const auto& info = licenseManager.getLicenseInfo();

        std::string maskedKey = info.licenseKey;
        if (maskedKey.length() > 8) {
            maskedKey = maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
        }

        // Status indicator
        NSImageView* statusIcon = [[NSImageView alloc] initWithFrame:NSMakeRect(24, 180, 20, 20)];
        statusIcon.image = [NSImage imageWithSystemSymbolName:@"checkmark.circle.fill"
                                     accessibilityDescription:@"Active"];
        statusIcon.contentTintColor = [NSColor systemGreenColor];
        [contentView addSubview:statusIcon];

        NSTextField* statusLabel = [NSTextField labelWithString:@"License Active"];
        statusLabel.frame = NSMakeRect(48, 178, 200, 24);
        statusLabel.font = [NSFont boldSystemFontOfSize:16];
        [contentView addSubview:statusLabel];

        // Email label
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

        // Key label
        NSTextField* keyLabel = [NSTextField labelWithString:@"Key:"];
        keyLabel.frame = NSMakeRect(24, 120, 80, 20);
        keyLabel.textColor = [NSColor secondaryLabelColor];
        keyLabel.alignment = NSTextAlignmentRight;
        [contentView addSubview:keyLabel];

        NSTextField* keyValueLabel =
            [NSTextField labelWithString:[NSString stringWithUTF8String:maskedKey.c_str()]];
        keyValueLabel.frame = NSMakeRect(112, 120, 310, 20);
        [contentView addSubview:keyValueLabel];

        // Machine ID label
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

        // Status message label (for errors)
        NSTextField* statusMessageLabel = [NSTextField labelWithString:@""];
        statusMessageLabel.frame = NSMakeRect(24, 65, 400, 20);
        statusMessageLabel.textColor = [NSColor systemRedColor];
        [contentView addSubview:statusMessageLabel];

        // Buttons
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

        // Store context in the button using associated objects
        objc_setAssociatedObject(deactivateButton, "dialog", dialog, OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(deactivateButton, "statusLabel", statusMessageLabel,
                                 OBJC_ASSOCIATION_RETAIN);

        [deactivateButton setTarget:self];
        [deactivateButton setAction:@selector(deactivateLicenseFromDialog:)];

    } else {
        // Unlicensed view
        NSTextField* titleLabel = [NSTextField labelWithString:@"Register License"];
        titleLabel.frame = NSMakeRect(24, 185, 400, 24);
        titleLabel.font = [NSFont boldSystemFontOfSize:16];
        [contentView addSubview:titleLabel];

        NSTextField* descLabel =
            [NSTextField labelWithString:@"Enter your license key to activate DearSQL:"];
        descLabel.frame = NSMakeRect(24, 160, 400, 20);
        [contentView addSubview:descLabel];

        // License key text field
        NSTextField* keyField = [[NSTextField alloc] initWithFrame:NSMakeRect(24, 125, 400, 28)];
        keyField.placeholderString = @"XXXX-XXXX-XXXX-XXXX";
        keyField.editable = YES;
        keyField.selectable = YES;
        keyField.bezeled = YES;
        keyField.bezelStyle = NSTextFieldRoundedBezel;
        [contentView addSubview:keyField];

        // Store keyField to make it first responder later
        objc_setAssociatedObject(dialog, "keyField", keyField, OBJC_ASSOCIATION_RETAIN);

        // Machine ID label
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

        // Status message label
        NSTextField* statusMessageLabel = [NSTextField labelWithString:@""];
        statusMessageLabel.frame = NSMakeRect(24, 70, 400, 20);
        statusMessageLabel.textColor = [NSColor systemRedColor];
        [contentView addSubview:statusMessageLabel];

        // Purchase link
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

        // Buttons
        NSButton* cancelButton = [[NSButton alloc] initWithFrame:NSMakeRect(250, 16, 90, 32)];
        cancelButton.title = @"Cancel";
        cancelButton.bezelStyle = NSBezelStyleRounded;
        cancelButton.target = dialog;
        cancelButton.action = @selector(close);
        [contentView addSubview:cancelButton];

        NSButton* activateButton = [[NSButton alloc] initWithFrame:NSMakeRect(345, 16, 90, 32)];
        activateButton.title = @"Activate";
        activateButton.bezelStyle = NSBezelStyleRounded;
        activateButton.keyEquivalent = @"\r"; // Enter key
        [contentView addSubview:activateButton];

        // Store references for the callback
        objc_setAssociatedObject(activateButton, "dialog", dialog, OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(activateButton, "keyField", keyField, OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(activateButton, "statusLabel", statusMessageLabel,
                                 OBJC_ASSOCIATION_RETAIN);

        activateButton.target = self;
        activateButton.action = @selector(activateLicenseFromDialog:);
    }

    dialog.contentView = contentView;

    // Get the keyField if we're in unlicensed mode
    NSTextField* keyFieldToFocus = objc_getAssociatedObject(dialog, "keyField");

    // Set initial first responder
    if (keyFieldToFocus) {
        dialog.initialFirstResponder = keyFieldToFocus;
    }

    // Use modal window instead of sheet for proper keyboard support (Cmd+V paste)
    if (mainWindow) {
        [dialog setLevel:NSModalPanelWindowLevel];
        NSRect mainFrame = mainWindow.frame;
        NSRect dialogFrame = dialog.frame;
        CGFloat x = NSMidX(mainFrame) - dialogFrame.size.width / 2;
        CGFloat y = NSMidY(mainFrame) - dialogFrame.size.height / 2;
        [dialog setFrameOrigin:NSMakePoint(x, y)];
    }

    [dialog makeKeyAndOrderFront:nil];
    if (keyFieldToFocus) {
        [dialog makeFirstResponder:keyFieldToFocus];
    }
}

- (void)reportBugClicked:(id)sender {
    [self.menuPopover close];
    NSString* version = @APP_VERSION;
    NSString* urlStr = [NSString
        stringWithFormat:@"https://github.com/dunkbing/dearsql-website/issues/new?labels=bug"
                          "&title=%%5BBug%%5D+&body=%%23%%23+Description%%0A%%0A%%23%%23+Steps+"
                          "to+Reproduce%%0A1.+%%0A2.+%%0A3.+%%0A%%0A%%23%%23+Expected+Behavior"
                          "%%0A%%0A%%23%%23+Actual+Behavior%%0A%%0A%%23%%23+Environment%%0A-+**OS"
                          "**%%3A+macOS%%0A-+**DearSQL+version**%%3A+%@%%0A-+**Database**%%3A+",
                         version];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:urlStr]];
}

- (void)checkForUpdatesClicked:(id)sender {
    [self.menuPopover close];
    checkForUpdates();
}

- (void)openPurchaseLink:(id)sender {
    NSURL* url = [NSURL
        URLWithString:
            @"https://dearsql.lemonsqueezy.com/checkout/buy/8d4644a9-dfcb-4a06-aeab-a8890d082673"];
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

    // Retain objects for async operation (MRC)
    [dialog retain];
    [sender retain];
    [statusLabel retain];

    LicenseManager::instance().activateLicense(
        licenseKey, [dialog, sender, statusLabel](const LicenseInfo& result) {
            NSLog(@"License activation callback: valid=%d, status=%s, error=%s", result.valid,
                  result.status.c_str(), result.error.c_str());

            // Capture result values before dispatch since result may go out of scope
            bool isValid = result.valid;
            std::string errorMsg = result.error;

            dispatch_async(dispatch_get_main_queue(), ^{
              NSLog(@"License activation main thread: isValid=%d", isValid);
              if (isValid) {
                  // Success - close dialog
                  NSLog(@"License activation: closing dialog");
                  [dialog close];
              } else {
                  // Error
                  std::string err = errorMsg.empty() ? "Activation failed" : errorMsg;
                  NSLog(@"License activation: showing error - %s", err.c_str());
                  statusLabel.stringValue = [NSString stringWithUTF8String:err.c_str()];
                  statusLabel.textColor = [NSColor systemRedColor];
                  sender.enabled = YES;
              }
              // Release retained objects
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

    // Retain objects for async operation (MRC)
    [dialog retain];
    [sender retain];
    [statusLabel retain];

    LicenseManager::instance().deactivateLicense(
        [dialog, sender, statusLabel](const LicenseInfo& result) {
            // Capture result values before dispatch since result may go out of scope
            std::string errorMsg = result.error;

            dispatch_async(dispatch_get_main_queue(), ^{
              if (errorMsg.empty()) {
                  // Success - close dialog
                  [dialog close];
              } else {
                  // Error
                  statusLabel.stringValue = [NSString stringWithUTF8String:errorMsg.c_str()];
                  statusLabel.textColor = [NSColor systemRedColor];
                  sender.enabled = YES;
              }
              // Release retained objects
              [dialog release];
              [sender release];
              [statusLabel release];
            });
        });
}

@end

MacOSPlatform::MacOSPlatform(Application* app) : app_(app), window_(nullptr) {
    toolbarDelegate_ = nullptr;
    metalDevice_ = nullptr;
    metalCommandQueue_ = nullptr;
    metalLayer_ = nullptr;
    visualEffectView_ = nullptr;
}

MacOSPlatform::~MacOSPlatform() {
    cleanup();
}

bool MacOSPlatform::initializePlatform(GLFWwindow* window) {
    window_ = window;

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

    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    NSView* contentView = nsWindow.contentView;

    // Make the contentView layer-backed so subviews composite correctly.
    // Do NOT set a custom layer here — GLFW's contentView stays as-is for events.
    [contentView setWantsLayer:YES];

    // --- Vibrancy blur (behind everything) ---
    PassthroughEffectView* effectView =
        [[PassthroughEffectView alloc] initWithFrame:contentView.bounds];
    effectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    effectView.state = NSVisualEffectStateActive;
    effectView.material = NSVisualEffectMaterialUnderWindowBackground;
    effectView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [contentView addSubview:effectView];
    visualEffectView_ = effectView;

    // --- Metal rendering view (on top of blur, pass-through for events) ---
    // Wrapped in a layer-backed container whose alphaValue lets blur show through.
    // (alphaValue is ignored on layer-hosting views, so the extra wrapper is needed.)
    PassthroughView* appContainer = [[PassthroughView alloc] initWithFrame:contentView.bounds];
    [appContainer setWantsLayer:YES];
    appContainer.layer.opaque = NO;
    appContainer.alphaValue = 0.85;
    appContainer.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    PassthroughView* metalView = [[PassthroughView alloc] initWithFrame:appContainer.bounds];
    metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = (id<MTLDevice>)metalDevice_;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = YES; // vsync
    layer.opaque = NO;
    metalView.layer = layer;
    [metalView setWantsLayer:YES];
    metalLayer_ = layer;

    [appContainer addSubview:metalView];
    [contentView addSubview:appContainer];

    std::cout << "Metal device and vibrancy layer initialized successfully" << std::endl;

    return true;
}

bool MacOSPlatform::initializeImGuiBackend() {
    ImGui_ImplMetal_Init((id<MTLDevice>)metalDevice_);
    std::cout << "ImGui Metal backend initialized" << std::endl;
    return true;
}

void MacOSPlatform::setupTitlebar() {
    // Get the native NSWindow from GLFW
    NSWindow* nsWindow = glfwGetCocoaWindow(window_);
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

    // Create custom title bar accessory view with sidebar and plus buttons
    NSView* buttonContainer = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 70, 0)];

    // Sidebar toggle button
    NSButton* sidebarButton = [[NSButton alloc] initWithFrame:NSMakeRect(0, 10, 30, 30)];
    [sidebarButton setImage:[NSImage imageWithSystemSymbolName:@"sidebar.left"
                                      accessibilityDescription:@"Toggle Sidebar"]];
    [sidebarButton setButtonType:NSButtonTypeMomentaryPushIn];
    [sidebarButton setBezelStyle:NSBezelStyleTexturedRounded];
    [sidebarButton setTarget:toolbarDelegate_];
    [sidebarButton setAction:@selector(sidebarToggleClicked:)];
    [sidebarButton setBordered:NO];
    [buttonContainer addSubview:sidebarButton];

    // Plus button to add database connection
    NSButton* plusButton = [[NSButton alloc] initWithFrame:NSMakeRect(32, 10, 30, 30)];
    [plusButton setImage:[NSImage imageWithSystemSymbolName:@"plus"
                                   accessibilityDescription:@"Add Database Connection"]];
    [plusButton setButtonType:NSButtonTypeMomentaryPushIn];
    [plusButton setBezelStyle:NSBezelStyleTexturedRounded];
    [plusButton setTarget:toolbarDelegate_];
    [plusButton setAction:@selector(connectButtonClicked:)];
    [plusButton setBordered:NO];
    [buttonContainer addSubview:plusButton];

    NSTitlebarAccessoryViewController* accessoryController =
        [[NSTitlebarAccessoryViewController alloc] init];
    accessoryController.view = buttonContainer;
    accessoryController.layoutAttribute = NSLayoutAttributeLeading;

    [nsWindow addTitlebarAccessoryViewController:accessoryController];

    // Toolbar for workspace selector and menu button
    NSToolbar* toolbar = [[NSToolbar alloc] initWithIdentifier:@"MainToolbar"];
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    toolbar.delegate = toolbarDelegate_;

    [nsWindow setToolbar:toolbar];

    std::cout << "Custom titlebar and toolbar configured" << std::endl;

    // Set background color to match app theme
    const auto& colors = app_->isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    NSColor* bgColor = [NSColor colorWithRed:colors.base.x
                                       green:colors.base.y
                                        blue:colors.base.z
                                       alpha:colors.base.w];
    [nsWindow setBackgroundColor:bgColor];

    // Set window appearance to match app theme
    NSAppearanceName appearanceName =
        app_->isDarkTheme() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    nsWindow.appearance = [NSAppearance appearanceNamed:appearanceName];

    std::cout << "Titlebar configured successfully" << std::endl;
}

float MacOSPlatform::getTitlebarHeight() const {
    NSWindow* nsWindow = glfwGetCocoaWindow(window_);
    if (!nsWindow) {
        return 0.0f;
    }

    // Get the titlebar height
    NSRect frame = [nsWindow frame];
    NSRect contentRect = [nsWindow contentRectForFrameRect:frame];
    return static_cast<float>(frame.size.height - contentRect.size.height);
}

void MacOSPlatform::onSidebarToggleClicked() {
    std::cout << "Sidebar toggle clicked" << std::endl;
    try {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    } catch (const std::exception& e) {
        std::cerr << "Exception in onSidebarToggleClicked: " << e.what() << std::endl;
    }
}

void MacOSPlatform::cleanup() {
    if (toolbarDelegate_) {
        toolbarDelegate_ = nullptr;
    }
    metalDevice_ = nullptr;
    metalCommandQueue_ = nullptr;
    metalLayer_ = nullptr;
    visualEffectView_ = nullptr;
}

void MacOSPlatform::renderFrame() {
    @autoreleasepool {
        // Get the Metal drawable
        id<CAMetalDrawable> drawable = [(CAMetalLayer*)metalLayer_ nextDrawable];
        if (!drawable) {
            return;
        }

        // Create render pass descriptor
        MTLRenderPassDescriptor* renderPassDescriptor =
            [MTLRenderPassDescriptor renderPassDescriptor];
        renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
        renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        const auto& clearCol =
            app_->isDarkTheme() ? Theme::NATIVE_DARK.base : Theme::NATIVE_LIGHT.base;
        renderPassDescriptor.colorAttachments[0].clearColor =
            MTLClearColorMake(clearCol.x, clearCol.y, clearCol.z, clearCol.w);
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
        ((CAMetalLayer*)metalLayer_).drawableSize = CGSizeMake(display_w, display_h);

        // Render ImGui draw data
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

        // End encoding and present
        [renderEncoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

void MacOSPlatform::shutdownImGui() {
    ImGui_ImplMetal_Shutdown();
    std::cout << "ImGui Metal backend shutdown" << std::endl;
}

void MacOSPlatform::updateWorkspaceDropdown() {
    if (toolbarDelegate_) {
        [toolbarDelegate_ updateWorkspaceDropdown];
    }
}

ImTextureID MacOSPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    id<MTLDevice> device = (id<MTLDevice>)metalDevice_;
    if (!device || !pixels) {
        return ImTextureID{};
    }

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;

    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    if (!texture) {
        return ImTextureID{};
    }

    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:width * 4];

    return (ImTextureID)(intptr_t)(void*)texture;
}
