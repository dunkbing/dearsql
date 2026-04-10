#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "platform/alert.hpp"
#include "platform/titlebar.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "ui/input_dialog.hpp"
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

// Workspace row view — shows edit/delete buttons and a hover background
@interface WorkspaceRowView : NSView
@property(nonatomic, strong) NSButton* editButton;
@property(nonatomic, strong) NSButton* deleteButton;
@end

@implementation WorkspaceRowView
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* ta in self.trackingAreas.copy) {
        [self removeTrackingArea:ta];
    }
    NSTrackingArea* ta = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:ta];
}
- (void)mouseEntered:(NSEvent*)event {
    self.wantsLayer = YES;
    self.layer.backgroundColor =
        [[NSColor controlAccentColor] colorWithAlphaComponent:0.08].CGColor;
    self.layer.cornerRadius = 4;
    self.editButton.hidden = NO;
    self.deleteButton.hidden = NO;
}
- (void)mouseExited:(NSEvent*)event {
    self.layer.backgroundColor = [NSColor clearColor].CGColor;
    self.editButton.hidden = YES;
    self.deleteButton.hidden = YES;
}
@end

// Toolbar delegate interface
@interface MacOSTitlebarDelegate : NSObject <NSToolbarDelegate>
@property(nonatomic, assign) Application* app;
@property(nonatomic, strong) NSButton* workspaceButton;
@property(nonatomic, strong) NSPopover* workspacePopover;
@property(nonatomic, strong) NSPopover* menuPopover;
@property(nonatomic, strong) NSButton* themeLightButton;
@property(nonatomic, strong) NSButton* themeDarkButton;
@property(nonatomic, strong) NSButton* themeAutoButton;
- (void)showMenuPopover:(NSButton*)sender;
- (void)showWorkspacePopover:(NSButton*)sender;
- (void)editWorkspaceClicked:(NSButton*)sender;
- (void)updateThemeButtons;
- (void)updateWindowBackgroundColor;
- (void)restoreMainWindowFocus:(NSWindow*)mainWindow;
- (void)updateWorkspaceDropdown;
@end

@implementation MacOSTitlebarDelegate
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

        self.workspaceButton = [[NSButton alloc] init];
        [self.workspaceButton setButtonType:NSButtonTypeMomentaryPushIn];
        [self.workspaceButton setBezelStyle:NSBezelStyleTexturedRounded];
        [self.workspaceButton setBordered:YES];
        [self.workspaceButton setTarget:self];
        [self.workspaceButton setAction:@selector(showWorkspacePopover:)];
        [self updateWorkspaceButtonLabel];
        [self.workspaceButton sizeToFit];

        item.view = self.workspaceButton;
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
            if (!self.app->canAddConnection()) {
                Alert::show(
                    "Connection Limit Reached",
                    "Free tier is limited to 3 connections. Activate a license to add more.");
                return;
            }
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

- (void)showWorkspacePopover:(NSButton*)sender {
    @try {
        [self updateWorkspaceDropdown];
        [self.workspacePopover showRelativeToRect:sender.bounds
                                           ofView:sender
                                    preferredEdge:NSRectEdgeMaxY];
    } @catch (NSException* exception) {
        NSLog(@"Exception in showWorkspacePopover: %@", exception);
    }
}

- (void)updateWorkspaceButtonLabel {
    if (!self.workspaceButton)
        return;
    if (self.app) {
        std::string name = self.app->getCurrentWorkspaceName();
        [self.workspaceButton setTitle:[NSString stringWithUTF8String:name.c_str()]];
    } else {
        [self.workspaceButton setTitle:@"Workspace"];
    }
    [self.workspaceButton sizeToFit];
}

- (void)updateWorkspaceDropdown {
    if (!self.app)
        return;

    [self updateWorkspaceButtonLabel];

    // rebuild popover
    if (!self.workspacePopover) {
        self.workspacePopover = [[NSPopover alloc] init];
        self.workspacePopover.behavior = NSPopoverBehaviorTransient;
    }

    auto workspaces = self.app->getWorkspaces();
    int currentWsId = self.app->getCurrentWorkspaceId();

    CGFloat w = 220;
    CGFloat rowH = 30;
    CGFloat padH = 8;
    CGFloat sepH = 1;
    CGFloat newBtnH = 30;
    // build bottom-up: pad + newBtn + pad + sep + pad + rows + pad
    CGFloat totalH = padH + newBtnH + padH + sepH + padH + (CGFloat)workspaces.size() * rowH + padH;

    NSView* contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w, totalH)];

    // layout from bottom up (AppKit y=0 is at the bottom)
    CGFloat y = padH;

    // "New Workspace..." button at the bottom
    NSButton* newWsBtn = [[NSButton alloc] initWithFrame:NSMakeRect(8, y, w - 16, newBtnH)];
    [newWsBtn setTitle:@"New Workspace..."];
    [newWsBtn setButtonType:NSButtonTypeMomentaryPushIn];
    [newWsBtn setBezelStyle:NSBezelStyleRounded];
    [newWsBtn setTarget:self];
    [newWsBtn setAction:@selector(createNewWorkspace:)];
    [contentView addSubview:newWsBtn];
    y += newBtnH + padH;

    // separator above button
    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(8, y, w - 16, sepH)];
    sep.boxType = NSBoxSeparator;
    [contentView addSubview:sep];
    y += sepH + padH;

    // workspace rows above separator (bottom-most workspace first)
    for (int i = (int)workspaces.size() - 1; i >= 0; i--) {
        const auto& ws = workspaces[i];
        int wsId = ws.id;
        bool isCurrent = wsId == currentWsId;

        WorkspaceRowView* row = [[WorkspaceRowView alloc] initWithFrame:NSMakeRect(0, y, w, rowH)];

        // checkmark icon for current workspace
        NSImageView* checkIcon =
            [[NSImageView alloc] initWithFrame:NSMakeRect(6, (rowH - 14) / 2, 14, 14)];
        checkIcon.image = [NSImage imageWithSystemSymbolName:@"checkmark"
                                    accessibilityDescription:@"Active"];
        checkIcon.hidden = !isCurrent;
        [row addSubview:checkIcon];

        // edit button (hidden by default, revealed on hover)
        NSButton* editBtn =
            [[NSButton alloc] initWithFrame:NSMakeRect(w - 52, (rowH - 22) / 2, 22, 22)];
        [editBtn setImage:[NSImage imageWithSystemSymbolName:@"pencil"
                                    accessibilityDescription:@"Rename Workspace"]];
        [editBtn setButtonType:NSButtonTypeMomentaryPushIn];
        [editBtn setBezelStyle:NSBezelStyleInline];
        [editBtn setBordered:NO];
        editBtn.hidden = YES;
        row.editButton = editBtn;

        objc_setAssociatedObject(editBtn, "wsId", @(wsId), OBJC_ASSOCIATION_RETAIN);
        objc_setAssociatedObject(editBtn, "wsName", [NSString stringWithUTF8String:ws.name.c_str()],
                                 OBJC_ASSOCIATION_RETAIN);
        [editBtn setTarget:self];
        [editBtn setAction:@selector(editWorkspaceClicked:)];
        [row addSubview:editBtn];

        // trash button (hidden by default, revealed on hover)
        NSButton* trashBtn =
            [[NSButton alloc] initWithFrame:NSMakeRect(w - 28, (rowH - 22) / 2, 22, 22)];
        [trashBtn setImage:[NSImage imageWithSystemSymbolName:@"trash"
                                     accessibilityDescription:@"Delete Workspace"]];
        [trashBtn setButtonType:NSButtonTypeMomentaryPushIn];
        [trashBtn setBezelStyle:NSBezelStyleInline];
        [trashBtn setBordered:NO];
        [trashBtn setContentTintColor:[NSColor systemRedColor]];
        trashBtn.hidden = YES;
        row.deleteButton = trashBtn;

        objc_setAssociatedObject(trashBtn, "wsId", @(wsId), OBJC_ASSOCIATION_RETAIN);
        [trashBtn setTarget:self];
        [trashBtn setAction:@selector(deleteWorkspaceClicked:)];
        [row addSubview:trashBtn];

        // name button — fills space between checkmark and action buttons
        CGFloat nameX = 24;
        CGFloat nameW = w - nameX - 56;
        NSButton* nameBtn = [[NSButton alloc] initWithFrame:NSMakeRect(nameX, 1, nameW, rowH - 2)];
        [nameBtn setTitle:[NSString stringWithUTF8String:ws.name.c_str()]];
        [nameBtn setButtonType:NSButtonTypeMomentaryPushIn];
        [nameBtn setBezelStyle:NSBezelStyleInline];
        [nameBtn setBordered:NO];
        nameBtn.alignment = NSTextAlignmentLeft;
        objc_setAssociatedObject(nameBtn, "wsId", @(wsId), OBJC_ASSOCIATION_RETAIN);
        [nameBtn setTarget:self];
        [nameBtn setAction:@selector(selectWorkspaceClicked:)];
        [row addSubview:nameBtn];

        [contentView addSubview:row];
        y += rowH;
    }

    NSViewController* vc = [[NSViewController alloc] init];
    vc.view = contentView;
    self.workspacePopover.contentViewController = vc;
    self.workspacePopover.contentSize = NSMakeSize(w, totalH);
}

- (void)selectWorkspaceClicked:(NSButton*)sender {
    @try {
        NSNumber* wsIdNum = objc_getAssociatedObject(sender, "wsId");
        if (wsIdNum && self.app) {
            self.app->setCurrentWorkspace(wsIdNum.intValue);
            [self updateWorkspaceButtonLabel];
        }
        [self.workspacePopover close];
    } @catch (NSException* exception) {
        NSLog(@"Exception in selectWorkspaceClicked: %@", exception);
    }
}

- (void)deleteWorkspaceClicked:(NSButton*)sender {
    @try {
        NSNumber* wsIdNum = objc_getAssociatedObject(sender, "wsId");
        if (!wsIdNum || !self.app)
            return;
        int wsId = wsIdNum.intValue;
        [self.workspacePopover close];
        Alert::show("Delete Workspace",
                    "Are you sure you want to delete this workspace? "
                    "All connections in this workspace will be removed.",
                    {AlertButton{"Delete",
                                 [self, wsId]() {
                                     if (self.app) {
                                         self.app->deleteWorkspace(wsId);
                                         [self updateWorkspaceButtonLabel];
                                     }
                                 },
                                 AlertButton::Style::Destructive},
                     AlertButton{"Cancel", {}, AlertButton::Style::Cancel}});
    } @catch (NSException* exception) {
        NSLog(@"Exception in deleteWorkspaceClicked: %@", exception);
    }
}

- (void)editWorkspaceClicked:(NSButton*)sender {
    @try {
        NSNumber* wsIdNum = objc_getAssociatedObject(sender, "wsId");
        NSString* wsName = objc_getAssociatedObject(sender, "wsName");
        if (!wsIdNum || !self.app)
            return;
        int wsId = wsIdNum.intValue;
        std::string currentName = wsName ? [wsName UTF8String] : "";
        [self.workspacePopover close];
        InputDialog::show("Rename Workspace", "", currentName, "Rename",
                          [self, wsId](const std::string& newName) -> std::string {
                              if (newName.empty())
                                  return "Name cannot be empty.";
                              self.app->renameWorkspace(wsId, newName);
                              [self updateWorkspaceButtonLabel];
                              return "";
                          });
    } @catch (NSException* exception) {
        NSLog(@"Exception in editWorkspaceClicked: %@", exception);
    }
}

- (void)createNewWorkspace:(id)sender {
    @try {
        if (!self.app)
            return;

        [self.workspacePopover close];

        if (!self.app->canAddWorkspace()) {
            Alert::show("Workspace Limit Reached",
                        "Free tier is limited to 2 workspaces. Activate a license to create more.");
            return;
        }

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
                                    int result = self.app->createWorkspace(name);
                                    if (result <= 0) {
                                        NSLog(@"Failed to create workspace: %@", workspaceName);
                                    }
                                }
                            }
                            [self updateWorkspaceButtonLabel];
                            [mainWindow displayIfNeeded];
                            [self restoreMainWindowFocus:mainWindow];
                          }];
        } else {
            // fallback to modal dialog if no main window
            NSModalResponse returnCode = [alert runModal];
            if (returnCode == NSAlertFirstButtonReturn) {
                NSString* workspaceName = textField.stringValue;
                if (workspaceName.length > 0 && self.app) {
                    std::string name = [workspaceName UTF8String];
                    int result = self.app->createWorkspace(name);
                    if (result <= 0) {
                        NSLog(@"Failed to create workspace: %@", workspaceName);
                    }
                }
            }
            [self updateWorkspaceButtonLabel];
            [self restoreMainWindowFocus:mainWindow];
        }
    } @catch (NSException* exception) {
        NSLog(@"Exception in createNewWorkspace: %@", exception);
    }
}

- (void)restoreMainWindowFocus:(NSWindow*)mainWindow {
    if (!mainWindow) {
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      [NSApp activateIgnoringOtherApps:YES];
      [mainWindow makeKeyAndOrderFront:nil];
    });
}

- (void)showMenuPopover:(NSButton*)sender {
    @try {
        if (!self.menuPopover) {
            [self createMenuPopover];
        }
        [self updateThemeButtons];
        [self updateFontSizeLabel];
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

    NSViewController* contentVC = [[NSViewController alloc] init];

    // layout from bottom up: margin=12, rowH=28, labelH=16, gap=8
    CGFloat y = 12;
    CGFloat contentW = 180;
    CGFloat rowW = 156;
    CGFloat rowX = 12;

    NSView* contentView =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, contentW, 0)]; // resized below

    // Report Bug button
    NSButton* reportBugButton = [[NSButton alloc] initWithFrame:NSMakeRect(rowX, y, rowW, 28)];
    [reportBugButton setTitle:@"Report Bug..."];
    [reportBugButton setButtonType:NSButtonTypeMomentaryPushIn];
    [reportBugButton setBezelStyle:NSBezelStyleTexturedRounded];
    [reportBugButton setTarget:self];
    [reportBugButton setAction:@selector(reportBugClicked:)];
    [contentView addSubview:reportBugButton];
    y += 28 + 8;

    // Check for Updates button
    NSButton* updateButton = [[NSButton alloc] initWithFrame:NSMakeRect(rowX, y, rowW, 28)];
    [updateButton setTitle:@"Check for Updates..."];
    [updateButton setButtonType:NSButtonTypeMomentaryPushIn];
    [updateButton setBezelStyle:NSBezelStyleTexturedRounded];
    [updateButton setTarget:self];
    [updateButton setAction:@selector(checkForUpdatesClicked:)];
    [contentView addSubview:updateButton];
    y += 28 + 8;

    // License button
    NSButton* licenseButton = [[NSButton alloc] initWithFrame:NSMakeRect(rowX, y, rowW, 28)];
    [licenseButton setTitle:@"Manage License"];
    [licenseButton setButtonType:NSButtonTypeMomentaryPushIn];
    [licenseButton setBezelStyle:NSBezelStyleTexturedRounded];
    [licenseButton setTarget:self];
    [licenseButton setAction:@selector(licenseClicked:)];
    [contentView addSubview:licenseButton];
    y += 28 + 8;

    // Separator line
    NSBox* separator = [[NSBox alloc] initWithFrame:NSMakeRect(rowX, y, rowW, 1)];
    separator.boxType = NSBoxSeparator;
    [contentView addSubview:separator];
    y += 1 + 8;

    // Font size section
    CGFloat fontBtnW = 40;
    CGFloat fontSpacing = 4;

    NSButton* fontDecButton = [[NSButton alloc] initWithFrame:NSMakeRect(rowX, y, fontBtnW, 28)];
    [fontDecButton setTitle:@"A-"];
    [fontDecButton setButtonType:NSButtonTypeMomentaryPushIn];
    [fontDecButton setBezelStyle:NSBezelStyleTexturedRounded];
    [fontDecButton setTarget:self];
    [fontDecButton setAction:@selector(fontSizeDecClicked:)];
    [fontDecButton setToolTip:@"Decrease Font Size"];
    [contentView addSubview:fontDecButton];

    NSTextField* fontSizeValueLabel = [NSTextField labelWithString:@"100%"];
    fontSizeValueLabel.alignment = NSTextAlignmentCenter;
    CGFloat fontLabelW = rowW - 2 * (fontBtnW + fontSpacing);
    CGFloat fontLabelH = 18;
    CGFloat fontLabelY = y + (28 - fontLabelH) / 2;
    fontSizeValueLabel.frame =
        NSMakeRect(rowX + fontBtnW + fontSpacing, fontLabelY, fontLabelW, fontLabelH);
    [contentView addSubview:fontSizeValueLabel];
    objc_setAssociatedObject(self, "fontSizeLabel", fontSizeValueLabel, OBJC_ASSOCIATION_RETAIN);

    NSButton* fontIncButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(rowX + rowW - fontBtnW, y, fontBtnW, 28)];
    [fontIncButton setTitle:@"A+"];
    [fontIncButton setButtonType:NSButtonTypeMomentaryPushIn];
    [fontIncButton setBezelStyle:NSBezelStyleTexturedRounded];
    [fontIncButton setTarget:self];
    [fontIncButton setAction:@selector(fontSizeIncClicked:)];
    [fontIncButton setToolTip:@"Increase Font Size"];
    [contentView addSubview:fontIncButton];
    y += 28 + 4;

    NSTextField* fontSizeLabel = [NSTextField labelWithString:@"Font Size"];
    fontSizeLabel.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    fontSizeLabel.textColor = [NSColor secondaryLabelColor];
    fontSizeLabel.frame = NSMakeRect(rowX, y, rowW, 16);
    [contentView addSubview:fontSizeLabel];
    y += 16 + 8;

    // Theme buttons
    CGFloat buttonWidth = 50;
    CGFloat buttonHeight = 28;
    CGFloat spacing = 4;

    self.themeLightButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(rowX, y, buttonWidth, buttonHeight)];
    [self.themeLightButton setImage:[NSImage imageWithSystemSymbolName:@"sun.max"
                                              accessibilityDescription:@"Light Theme"]];
    [self.themeLightButton setButtonType:NSButtonTypeMomentaryPushIn];
    [self.themeLightButton setBezelStyle:NSBezelStyleTexturedRounded];
    [self.themeLightButton setTarget:self];
    [self.themeLightButton setAction:@selector(themeLightClicked:)];
    [self.themeLightButton setToolTip:@"Light"];
    [contentView addSubview:self.themeLightButton];

    self.themeDarkButton = [[NSButton alloc]
        initWithFrame:NSMakeRect(rowX + buttonWidth + spacing, y, buttonWidth, buttonHeight)];
    [self.themeDarkButton setImage:[NSImage imageWithSystemSymbolName:@"moon"
                                             accessibilityDescription:@"Dark Theme"]];
    [self.themeDarkButton setButtonType:NSButtonTypeMomentaryPushIn];
    [self.themeDarkButton setBezelStyle:NSBezelStyleTexturedRounded];
    [self.themeDarkButton setTarget:self];
    [self.themeDarkButton setAction:@selector(themeDarkClicked:)];
    [self.themeDarkButton setToolTip:@"Dark"];
    [contentView addSubview:self.themeDarkButton];

    self.themeAutoButton = [[NSButton alloc]
        initWithFrame:NSMakeRect(rowX + 2 * (buttonWidth + spacing), y, buttonWidth, buttonHeight)];
    [self.themeAutoButton setImage:[NSImage imageWithSystemSymbolName:@"circle.lefthalf.filled"
                                             accessibilityDescription:@"Auto Theme"]];
    [self.themeAutoButton setButtonType:NSButtonTypeMomentaryPushIn];
    [self.themeAutoButton setBezelStyle:NSBezelStyleTexturedRounded];
    [self.themeAutoButton setTarget:self];
    [self.themeAutoButton setAction:@selector(themeAutoClicked:)];
    [self.themeAutoButton setToolTip:@"System"];
    [contentView addSubview:self.themeAutoButton];
    y += buttonHeight + 4;

    // Theme section label
    NSTextField* themeLabel = [NSTextField labelWithString:@"Theme"];
    themeLabel.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    themeLabel.textColor = [NSColor secondaryLabelColor];
    themeLabel.frame = NSMakeRect(rowX, y, rowW, 16);
    [contentView addSubview:themeLabel];
    y += 16 + 8;

    // resize content view to fit
    [contentView setFrameSize:NSMakeSize(contentW, y)];

    contentVC.view = contentView;
    self.menuPopover.contentViewController = contentVC;
}

- (void)updateThemeButtons {
    if (!self.app || !self.themeLightButton || !self.themeDarkButton || !self.themeAutoButton) {
        return;
    }

    bool isDark = self.app->isDarkTheme();

    self.themeLightButton.wantsLayer = YES;
    self.themeDarkButton.wantsLayer = YES;
    self.themeAutoButton.wantsLayer = YES;

    // reset all buttons to default appearance
    [self.themeLightButton setContentTintColor:nil];
    [self.themeDarkButton setContentTintColor:nil];
    [self.themeAutoButton setContentTintColor:nil];
    for (NSButton* btn in @[ self.themeLightButton, self.themeDarkButton, self.themeAutoButton ]) {
        for (CALayer* sub in [btn.layer.sublayers copy]) {
            if ([sub.name isEqualToString:@"themeHighlight"])
                [sub removeFromSuperlayer];
        }
    }

    // highlight the currently selected theme with an inset sublayer
    NSButton* selectedButton = isDark ? self.themeDarkButton : self.themeLightButton;
    [selectedButton setContentTintColor:[NSColor controlAccentColor]];

    CALayer* highlight = [CALayer layer];
    highlight.name = @"themeHighlight";
    highlight.frame = NSInsetRect(selectedButton.bounds, 1, 3);
    highlight.backgroundColor = [[NSColor controlAccentColor] colorWithAlphaComponent:0.15].CGColor;
    highlight.cornerRadius = 5;
    [selectedButton.layer insertSublayer:highlight atIndex:0];
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

    // update window appearance so native titlebar controls match the theme
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

- (void)updateFontSizeLabel {
    NSTextField* label = objc_getAssociatedObject(self, "fontSizeLabel");
    if (label && self.app) {
        int pct = static_cast<int>(self.app->getFontScale() * 100);
        label.stringValue = [NSString stringWithFormat:@"%d%%", pct];
    }
}

- (void)fontSizeDecClicked:(id)sender {
    if (self.app) {
        self.app->setFontScale(self.app->getFontScale() - 0.1f);
        [self updateFontSizeLabel];
    }
}

- (void)fontSizeIncClicked:(id)sender {
    if (self.app) {
        self.app->setFontScale(self.app->getFontScale() + 0.1f);
        [self updateFontSizeLabel];
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
    [self.menuPopover close];
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

- (void)checkForUpdatesClicked:(id)sender {
    [self.menuPopover close];
    checkForUpdates();
}

- (void)openPurchaseLink:(id)sender {
    NSURL* url = [NSURL
        URLWithString:@"https://buy.polar.sh/polar_cl_IpYdAWiNljfzsXgatypm2mg40Mm2c4hB0DcVX1L9P6p"];
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

// ---- MacOSTitlebar C++ implementation ----

MacOSTitlebar::MacOSTitlebar(Application* app, GLFWwindow* window)
    : app_(app), window_(window), delegate_(nullptr) {
    delegate_ = [[MacOSTitlebarDelegate alloc] init];
    delegate_.app = app_;
}

MacOSTitlebar::~MacOSTitlebar() {
    delegate_ = nullptr;
}

void MacOSTitlebar::setup() {
    NSWindow* nsWindow = glfwGetCocoaWindow(window_);
    if (!nsWindow) {
        NSLog(@"Failed to get NSWindow from GLFW");
        return;
    }

    nsWindow.titlebarAppearsTransparent = YES;
    [nsWindow setStyleMask:[nsWindow styleMask]];

    // button container in the leading titlebar accessory area
    NSView* buttonContainer = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 70, 0)];

    // sidebar toggle button
    NSButton* sidebarButton = [[NSButton alloc] initWithFrame:NSMakeRect(0, 10, 30, 30)];
    [sidebarButton setImage:[NSImage imageWithSystemSymbolName:@"sidebar.left"
                                      accessibilityDescription:@"Toggle Sidebar"]];
    [sidebarButton setButtonType:NSButtonTypeMomentaryPushIn];
    [sidebarButton setBezelStyle:NSBezelStyleTexturedRounded];
    [sidebarButton setTarget:delegate_];
    [sidebarButton setAction:@selector(sidebarToggleClicked:)];
    [sidebarButton setBordered:NO];
    [buttonContainer addSubview:sidebarButton];

    // plus button to add database connection
    NSButton* plusButton = [[NSButton alloc] initWithFrame:NSMakeRect(32, 10, 30, 30)];
    [plusButton setImage:[NSImage imageWithSystemSymbolName:@"plus"
                                   accessibilityDescription:@"Add Database Connection"]];
    [plusButton setButtonType:NSButtonTypeMomentaryPushIn];
    [plusButton setBezelStyle:NSBezelStyleTexturedRounded];
    [plusButton setTarget:delegate_];
    [plusButton setAction:@selector(connectButtonClicked:)];
    [plusButton setBordered:NO];
    [buttonContainer addSubview:plusButton];

    NSTitlebarAccessoryViewController* accessoryController =
        [[NSTitlebarAccessoryViewController alloc] init];
    accessoryController.view = buttonContainer;
    accessoryController.layoutAttribute = NSLayoutAttributeLeading;

    [nsWindow addTitlebarAccessoryViewController:accessoryController];

    // toolbar for workspace selector and menu button
    NSToolbar* toolbar = [[NSToolbar alloc] initWithIdentifier:@"MainToolbar"];
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    toolbar.delegate = delegate_;

    [nsWindow setToolbar:toolbar];

    NSLog(@"Custom titlebar and toolbar configured");

    // set background color to match app theme
    const auto& colors = app_->isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    NSColor* bgColor = [NSColor colorWithRed:colors.base.x
                                       green:colors.base.y
                                        blue:colors.base.z
                                       alpha:colors.base.w];
    [nsWindow setBackgroundColor:bgColor];

    NSAppearanceName appearanceName =
        app_->isDarkTheme() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    nsWindow.appearance = [NSAppearance appearanceNamed:appearanceName];

    NSLog(@"Titlebar configured successfully");
}

void MacOSTitlebar::updateWorkspaceDropdown() {
    if (delegate_) {
        [delegate_ updateWorkspaceDropdown];
    }
}

void MacOSTitlebar::applyTheme(bool isDark) {
    if (!window_)
        return;

    NSWindow* nsWindow = glfwGetCocoaWindow(window_);
    if (!nsWindow)
        return;

    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
    NSColor* bgColor = [NSColor colorWithRed:colors.base.x
                                       green:colors.base.y
                                        blue:colors.base.z
                                       alpha:colors.base.w];
    [nsWindow setBackgroundColor:bgColor];

    NSAppearanceName appearanceName = isDark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    nsWindow.appearance = [NSAppearance appearanceNamed:appearanceName];

    if (delegate_) {
        [delegate_ updateThemeButtons];
    }
}
