#include "platform/macos_connection_dialog.hpp"
#include "app_state.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/query_executor.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "utils/file_dialog.hpp"

#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#import <GLFW/glfw3native.h>

static const CGFloat kDialogWidth = 500;
static const CGFloat kMargin = 20;
static const CGFloat kRowSpacing = 10;
static const CGFloat kRowHeight = 28;
static const CGFloat kLabelWidth = 80;
static const CGFloat kLabelGap = 8;
static const CGFloat kFieldX = kMargin + kLabelWidth + kLabelGap;
static const CGFloat kFieldWidth = kDialogWidth - kFieldX - kMargin;

// MARK: - ConnectionDialogController

@interface ConnectionDialogController : NSObject <NSWindowDelegate> {
    std::shared_ptr<DatabaseInterface> _editingDb;
    std::atomic<bool> _cancelled;
}

@property(nonatomic, assign) Application* app;
@property(nonatomic, strong) NSWindow* dialogWindow;
@property(nonatomic) int editingConnectionId;

// Always-visible controls
@property(nonatomic, strong) NSTextField* nameLabel;
@property(nonatomic, strong) NSTextField* nameField;
@property(nonatomic, strong) NSTextField* typeLabel;
@property(nonatomic, strong) NSPopUpButton* typePopup;
@property(nonatomic, strong) NSBox* topSeparator;

// SQLite
@property(nonatomic, strong) NSTextField* sqlitePathLabel;
@property(nonatomic, strong) NSTextField* sqlitePathField;
@property(nonatomic, strong) NSButton* browseButton;

// Server fields
@property(nonatomic, strong) NSTextField* hostLabel;
@property(nonatomic, strong) NSTextField* hostField;
@property(nonatomic, strong) NSTextField* portLabel;
@property(nonatomic, strong) NSTextField* portField;
@property(nonatomic, strong) NSTextField* databaseLabel;
@property(nonatomic, strong) NSTextField* databaseField;

// PostgreSQL SSL
@property(nonatomic, strong) NSTextField* sslModeLabel;
@property(nonatomic, strong) NSPopUpButton* sslModePopup;

// Auth
@property(nonatomic, strong) NSTextField* authLabel;
@property(nonatomic, strong) NSSegmentedControl* authSegment;
@property(nonatomic, strong) NSTextField* usernameLabel;
@property(nonatomic, strong) NSTextField* usernameField;
@property(nonatomic, strong) NSTextField* passwordLabel;
@property(nonatomic, strong) NSSecureTextField* passwordField;

// Show all databases
@property(nonatomic, strong) NSButton* showAllDbsCheckbox;

// Bottom controls
@property(nonatomic, strong) NSBox* bottomSeparator;
@property(nonatomic, strong) NSTextField* statusLabel;
@property(nonatomic, strong) NSProgressIndicator* spinner;
@property(nonatomic, strong) NSButton* connectButton;
@property(nonatomic, strong) NSButton* cancelButton;

- (void)showDialog;
- (void)showDialogForEdit:(std::shared_ptr<DatabaseInterface>)db connectionId:(int)connId;

@end

static NSWindow* sActiveConnectionDialog = nil;

@implementation ConnectionDialogController

- (instancetype)init {
    self = [super init];
    if (self) {
        _editingConnectionId = -1;
        _cancelled = false;
    }
    return self;
}

- (void)dealloc {
    _editingDb.reset();
    [super dealloc];
}

// MARK: - Dialog lifecycle

- (void)ensureEditMenu {
    NSMenu* mainMenu = [NSApp mainMenu];
    if (!mainMenu) {
        mainMenu = [[NSMenu alloc] init];
        [NSApp setMainMenu:mainMenu];
    }

    for (NSMenuItem* item in mainMenu.itemArray) {
        if ([item.title isEqualToString:@"Edit"])
            return;
    }

    NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
    editMenuItem.title = @"Edit";
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];

    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                                 action:@selector(undo:)
                                          keyEquivalent:@"z"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Redo"
                                                 action:@selector(redo:)
                                          keyEquivalent:@"Z"]];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Cut"
                                                 action:@selector(cut:)
                                          keyEquivalent:@"x"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                 action:@selector(copy:)
                                          keyEquivalent:@"c"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                 action:@selector(paste:)
                                          keyEquivalent:@"v"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                 action:@selector(selectAll:)
                                          keyEquivalent:@"a"]];

    editMenuItem.submenu = editMenu;
    [mainMenu addItem:editMenuItem];
}

- (void)showDialog {
    [self ensureEditMenu];
    [self buildControls];
    [self layoutFields];

    // Center on main window
    NSWindow* mainWindow = nil;
    if (self.app) {
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            mainWindow = glfwGetCocoaWindow(glfwWindow);
        }
    }

    if (mainWindow) {
        [self.dialogWindow setLevel:NSModalPanelWindowLevel];
        NSRect mainFrame = mainWindow.frame;
        NSRect dialogFrame = self.dialogWindow.frame;
        CGFloat x = NSMidX(mainFrame) - dialogFrame.size.width / 2;
        CGFloat y = NSMidY(mainFrame) - dialogFrame.size.height / 2;
        [self.dialogWindow setFrameOrigin:NSMakePoint(x, y)];
    }

    // Match app theme
    if (self.app) {
        NSAppearanceName appearanceName =
            self.app->isDarkTheme() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
        self.dialogWindow.appearance = [NSAppearance appearanceNamed:appearanceName];
    }

    [self.dialogWindow makeKeyAndOrderFront:nil];
}

- (void)showDialogForEdit:(std::shared_ptr<DatabaseInterface>)db connectionId:(int)connId {
    _editingDb = db;
    self.editingConnectionId = connId;

    [self showDialog];

    // Set window title and button
    [self.dialogWindow setTitle:@"Edit Connection"];
    [self.connectButton setTitle:@"Update"];

    // Populate fields from existing connection
    const auto& info = db->getConnectionInfo();
    self.nameField.stringValue = [NSString stringWithUTF8String:info.name.c_str()];

    // Set database type (disable changing it in edit mode)
    [self.typePopup selectItemAtIndex:static_cast<int>(info.type)];
    [self.typePopup setEnabled:NO];

    switch (info.type) {
    case DatabaseType::SQLITE:
        self.sqlitePathField.stringValue = [NSString stringWithUTF8String:info.path.c_str()];
        break;

    case DatabaseType::POSTGRESQL: {
        self.hostField.stringValue = [NSString stringWithUTF8String:info.host.c_str()];
        self.portField.stringValue = [NSString stringWithFormat:@"%d", info.port];
        self.databaseField.stringValue = [NSString stringWithUTF8String:info.database.c_str()];
        self.showAllDbsCheckbox.state =
            info.showAllDatabases ? NSControlStateValueOn : NSControlStateValueOff;

        // SSL mode
        static const char* sslModes[] = {"disable", "allow",     "prefer",
                                         "require", "verify-ca", "verify-full"};
        for (int i = 0; i < 6; i++) {
            if (info.sslmode == sslModes[i]) {
                [self.sslModePopup selectItemAtIndex:i];
                break;
            }
        }

        // Auth
        if (info.username.empty()) {
            self.authSegment.selectedSegment = 1; // None
        } else {
            self.authSegment.selectedSegment = 0;
            self.usernameField.stringValue = [NSString stringWithUTF8String:info.username.c_str()];
            self.passwordField.stringValue = [NSString stringWithUTF8String:info.password.c_str()];
        }
        break;
    }

    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
    case DatabaseType::MONGODB: {
        self.hostField.stringValue = [NSString stringWithUTF8String:info.host.c_str()];
        self.portField.stringValue = [NSString stringWithFormat:@"%d", info.port];
        self.databaseField.stringValue = [NSString stringWithUTF8String:info.database.c_str()];
        self.showAllDbsCheckbox.state =
            info.showAllDatabases ? NSControlStateValueOn : NSControlStateValueOff;

        if (info.username.empty() && info.password.empty()) {
            self.authSegment.selectedSegment = 1;
        } else {
            self.authSegment.selectedSegment = 0;
            self.usernameField.stringValue = [NSString stringWithUTF8String:info.username.c_str()];
            self.passwordField.stringValue = [NSString stringWithUTF8String:info.password.c_str()];
        }
        break;
    }

    case DatabaseType::REDIS: {
        self.hostField.stringValue = [NSString stringWithUTF8String:info.host.c_str()];
        self.portField.stringValue = [NSString stringWithFormat:@"%d", info.port];

        if (info.username.empty() && info.password.empty()) {
            self.authSegment.selectedSegment = 1;
        } else {
            self.authSegment.selectedSegment = 0;
            self.usernameField.stringValue = [NSString stringWithUTF8String:info.username.c_str()];
            self.passwordField.stringValue = [NSString stringWithUTF8String:info.password.c_str()];
        }
        break;
    }
    }

    [self layoutFields];
}

// MARK: - Build UI

- (void)buildControls {
    NSString* title = (_editingDb) ? @"Edit Connection" : @"Connect to Database";
    self.dialogWindow =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kDialogWidth, 400)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskFullSizeContentView
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.dialogWindow.titlebarAppearsTransparent = YES;
    self.dialogWindow.titleVisibility = NSWindowTitleHidden;
    [self.dialogWindow standardWindowButton:NSWindowMiniaturizeButton].hidden = YES;
    [self.dialogWindow standardWindowButton:NSWindowZoomButton].hidden = YES;
    self.dialogWindow.delegate = self;

    // Keep controller alive as long as the window exists
    objc_setAssociatedObject(self.dialogWindow, "controller", self, OBJC_ASSOCIATION_RETAIN);

    NSView* cv = self.dialogWindow.contentView;

    // Name
    self.nameLabel = [self makeLabel:@"Name"];
    [cv addSubview:self.nameLabel];
    self.nameField = [self makeTextField:@"Untitled connection"];
    [cv addSubview:self.nameField];

    // Type
    self.typeLabel = [self makeLabel:@"Type"];
    [cv addSubview:self.typeLabel];
    self.typePopup = [[NSPopUpButton alloc] init];
    [self.typePopup addItemWithTitle:@"SQLite"];
    [self.typePopup addItemWithTitle:@"PostgreSQL"];
    [self.typePopup addItemWithTitle:@"MySQL"];
    [self.typePopup addItemWithTitle:@"MariaDB"];
    [self.typePopup addItemWithTitle:@"Redis"];
    [self.typePopup addItemWithTitle:@"MongoDB"];
    [self.typePopup setTarget:self];
    [self.typePopup setAction:@selector(typeChanged:)];
    [cv addSubview:self.typePopup];

    // Top separator
    self.topSeparator = [[NSBox alloc] init];
    self.topSeparator.boxType = NSBoxSeparator;
    [cv addSubview:self.topSeparator];

    // SQLite path
    self.sqlitePathLabel = [self makeLabel:@"File"];
    [cv addSubview:self.sqlitePathLabel];
    self.sqlitePathField = [self makeTextField:@"Database file path"];
    [cv addSubview:self.sqlitePathField];
    self.browseButton = [[NSButton alloc] init];
    [self.browseButton setTitle:@"Browse…"];
    [self.browseButton setBezelStyle:NSBezelStyleRounded];
    [self.browseButton setTarget:self];
    [self.browseButton setAction:@selector(browseClicked:)];
    [cv addSubview:self.browseButton];

    // Host
    self.hostLabel = [self makeLabel:@"Host"];
    [cv addSubview:self.hostLabel];
    self.hostField = [self makeTextField:@"localhost"];
    self.hostField.stringValue = @"localhost";
    [cv addSubview:self.hostField];

    // Port
    self.portLabel = [self makeLabel:@"Port"];
    [cv addSubview:self.portLabel];
    self.portField = [self makeTextField:@"5432"];
    self.portField.stringValue = @"5432";
    [cv addSubview:self.portField];

    // Database
    self.databaseLabel = [self makeLabel:@"Database"];
    [cv addSubview:self.databaseLabel];
    self.databaseField = [self makeTextField:@"(optional)"];
    [cv addSubview:self.databaseField];

    // SSL Mode
    self.sslModeLabel = [self makeLabel:@"SSL Mode"];
    [cv addSubview:self.sslModeLabel];
    self.sslModePopup = [[NSPopUpButton alloc] init];
    [self.sslModePopup addItemWithTitle:@"disable"];
    [self.sslModePopup addItemWithTitle:@"allow"];
    [self.sslModePopup addItemWithTitle:@"prefer"];
    [self.sslModePopup addItemWithTitle:@"require"];
    [self.sslModePopup addItemWithTitle:@"verify-ca"];
    [self.sslModePopup addItemWithTitle:@"verify-full"];
    [self.sslModePopup selectItemAtIndex:2]; // prefer
    [cv addSubview:self.sslModePopup];

    // Auth
    self.authLabel = [self makeLabel:@"Auth"];
    [cv addSubview:self.authLabel];
    self.authSegment =
        [NSSegmentedControl segmentedControlWithLabels:@[ @"Username & Password", @"None" ]
                                          trackingMode:NSSegmentSwitchTrackingSelectOne
                                                target:self
                                                action:@selector(authChanged:)];
    self.authSegment.selectedSegment = 0;
    [cv addSubview:self.authSegment];

    // Username
    self.usernameLabel = [self makeLabel:@"Username"];
    [cv addSubview:self.usernameLabel];
    self.usernameField = [self makeTextField:@"Username"];
    [cv addSubview:self.usernameField];

    // Password
    self.passwordLabel = [self makeLabel:@"Password"];
    [cv addSubview:self.passwordLabel];
    self.passwordField = [[NSSecureTextField alloc] init];
    self.passwordField.placeholderString = @"Password";
    self.passwordField.bezeled = YES;
    self.passwordField.bezelStyle = NSTextFieldRoundedBezel;
    [cv addSubview:self.passwordField];

    // Show all databases
    self.showAllDbsCheckbox = [NSButton checkboxWithTitle:@"Show all databases"
                                                   target:nil
                                                   action:nil];
    [cv addSubview:self.showAllDbsCheckbox];

    // Bottom separator
    self.bottomSeparator = [[NSBox alloc] init];
    self.bottomSeparator.boxType = NSBoxSeparator;
    [cv addSubview:self.bottomSeparator];

    // Status label
    self.statusLabel = [NSTextField labelWithString:@""];
    self.statusLabel.textColor = [NSColor systemRedColor];
    [cv addSubview:self.statusLabel];

    // Spinner
    self.spinner = [[NSProgressIndicator alloc] init];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.displayedWhenStopped = NO;
    [cv addSubview:self.spinner];

    // Connect button
    NSString* connectTitle = (_editingDb) ? @"Update" : @"Connect";
    self.connectButton = [[NSButton alloc] init];
    [self.connectButton setTitle:connectTitle];
    [self.connectButton setBezelStyle:NSBezelStyleRounded];
    [self.connectButton setKeyEquivalent:@"\r"];
    [self.connectButton setTarget:self];
    [self.connectButton setAction:@selector(connectClicked:)];
    [cv addSubview:self.connectButton];

    // Cancel button
    self.cancelButton = [[NSButton alloc] init];
    [self.cancelButton setTitle:@"Cancel"];
    [self.cancelButton setBezelStyle:NSBezelStyleRounded];
    [self.cancelButton setKeyEquivalent:@"\033"]; // Escape
    [self.cancelButton setTarget:self];
    [self.cancelButton setAction:@selector(cancelClicked:)];
    [cv addSubview:self.cancelButton];
}

- (NSTextField*)makeLabel:(NSString*)text {
    NSTextField* label = [NSTextField labelWithString:text];
    label.alignment = NSTextAlignmentRight;
    label.textColor = [NSColor secondaryLabelColor];
    label.font = [NSFont systemFontOfSize:13];
    return label;
}

- (NSTextField*)makeTextField:(NSString*)placeholder {
    NSTextField* field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.bezeled = YES;
    field.bezelStyle = NSTextFieldRoundedBezel;
    field.editable = YES;
    field.selectable = YES;
    return field;
}

// MARK: - Layout

- (void)hideAllOptionalFields {
    for (NSView* v in @[
             self.sqlitePathLabel, self.sqlitePathField, self.browseButton, self.hostLabel,
             self.hostField, self.portLabel, self.portField, self.databaseLabel, self.databaseField,
             self.sslModeLabel, self.sslModePopup, self.authLabel, self.authSegment,
             self.usernameLabel, self.usernameField, self.passwordLabel, self.passwordField,
             self.showAllDbsCheckbox
         ]) {
        v.hidden = YES;
    }
}

- (DatabaseType)selectedDatabaseType {
    return static_cast<DatabaseType>([self.typePopup indexOfSelectedItem]);
}

- (CGFloat)computeRequiredHeight {
    CGFloat h = kMargin;
    h += kRowHeight + kRowSpacing; // Name
    h += kRowHeight + kRowSpacing; // Type
    h += 1 + kRowSpacing;          // Top separator

    DatabaseType type = [self selectedDatabaseType];
    bool authIsCredentials = (self.authSegment.selectedSegment == 0);

    if (type == DatabaseType::SQLITE) {
        h += kRowHeight + kRowSpacing; // Path + Browse
    } else {
        h += kRowHeight + kRowSpacing; // Host + Port
        if (type != DatabaseType::REDIS) {
            h += kRowHeight + kRowSpacing; // Database
        }
        if (type == DatabaseType::POSTGRESQL) {
            h += kRowHeight + kRowSpacing; // SSL Mode
        }
        h += kRowHeight + kRowSpacing; // Auth segment
        if (authIsCredentials) {
            h += kRowHeight + kRowSpacing; // Username + Password
        }
        if (type != DatabaseType::REDIS) {
            h += kRowHeight + kRowSpacing; // Show all databases
        }
    }

    h += 20 + kRowSpacing; // Status
    h += 1 + kRowSpacing;  // Bottom separator
    h += kRowHeight;       // Buttons
    h += kMargin;          // Bottom margin
    return h;
}

- (void)layoutFields {
    [self hideAllOptionalFields];

    CGFloat windowH = [self computeRequiredHeight];

    // Resize window keeping top edge stable
    NSRect frame = self.dialogWindow.frame;
    CGFloat topEdge = NSMaxY(frame);
    frame.size.height = windowH;
    frame.origin.y = topEdge - windowH;
    // Ensure content rect matches
    NSRect contentRect = [self.dialogWindow contentRectForFrameRect:frame];
    CGFloat contentH = contentRect.size.height;
    [self.dialogWindow setFrame:frame display:YES animate:NO];

    CGFloat y = contentH - kMargin;
    DatabaseType type = [self selectedDatabaseType];
    bool authIsCredentials = (self.authSegment.selectedSegment == 0);

    // Name row
    y -= kRowHeight;
    self.nameLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
    self.nameField.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
    y -= kRowSpacing;

    // Type row
    y -= kRowHeight;
    self.typeLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
    self.typePopup.frame = NSMakeRect(kFieldX, y, 160, kRowHeight);
    y -= kRowSpacing;

    // Top separator
    y -= 1;
    self.topSeparator.frame = NSMakeRect(kMargin, y, kDialogWidth - 2 * kMargin, 1);
    y -= kRowSpacing;

    if (type == DatabaseType::SQLITE) {
        // SQLite path + browse
        self.sqlitePathLabel.hidden = NO;
        self.sqlitePathField.hidden = NO;
        self.browseButton.hidden = NO;
        y -= kRowHeight;
        self.sqlitePathLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        CGFloat browseW = 80;
        self.sqlitePathField.frame = NSMakeRect(kFieldX, y, kFieldWidth - browseW - 8, kRowHeight);
        self.browseButton.frame =
            NSMakeRect(kFieldX + kFieldWidth - browseW, y, browseW, kRowHeight);
        y -= kRowSpacing;
    } else {
        // Host + Port on same row
        self.hostLabel.hidden = NO;
        self.hostField.hidden = NO;
        self.portLabel.hidden = NO;
        self.portField.hidden = NO;
        y -= kRowHeight;
        self.hostLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        CGFloat portW = 70;
        CGFloat portLabelW = 35;
        CGFloat hostW = kFieldWidth - portW - portLabelW - 8 - 8;
        self.hostField.frame = NSMakeRect(kFieldX, y, hostW, kRowHeight);
        self.portLabel.frame = NSMakeRect(kFieldX + hostW + 8, y, portLabelW, kRowHeight);
        self.portField.frame =
            NSMakeRect(kFieldX + hostW + 8 + portLabelW + 8, y, portW, kRowHeight);
        y -= kRowSpacing;

        // Database (not for Redis)
        if (type != DatabaseType::REDIS) {
            self.databaseLabel.hidden = NO;
            self.databaseField.hidden = NO;
            y -= kRowHeight;
            self.databaseLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
            self.databaseField.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);

            if (type == DatabaseType::POSTGRESQL) {
                self.databaseField.toolTip = @"Leave empty to use the default 'postgres' database";
            } else {
                self.databaseField.toolTip = nil;
            }
            y -= kRowSpacing;
        }

        // SSL Mode (PostgreSQL only)
        if (type == DatabaseType::POSTGRESQL) {
            self.sslModeLabel.hidden = NO;
            self.sslModePopup.hidden = NO;
            y -= kRowHeight;
            self.sslModeLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
            self.sslModePopup.frame = NSMakeRect(kFieldX, y, 150, kRowHeight);
            y -= kRowSpacing;
        }

        // Auth segment
        self.authLabel.hidden = NO;
        self.authSegment.hidden = NO;
        y -= kRowHeight;
        self.authLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.authSegment.frame = NSMakeRect(kFieldX, y, 260, kRowHeight);
        y -= kRowSpacing;

        // Username + Password (if auth enabled)
        if (authIsCredentials) {
            self.usernameLabel.hidden = NO;
            self.usernameField.hidden = NO;
            self.passwordLabel.hidden = NO;
            self.passwordField.hidden = NO;
            y -= kRowHeight;
            self.usernameLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
            CGFloat halfField = (kFieldWidth - 50) / 2;
            self.usernameField.frame = NSMakeRect(kFieldX, y, halfField, kRowHeight);
            self.passwordLabel.frame = NSMakeRect(kFieldX + halfField + 8, y, 50, kRowHeight);
            self.passwordField.frame = NSMakeRect(kFieldX + halfField + 8 + 50 + 8, y,
                                                  kFieldWidth - halfField - 8 - 50 - 8, kRowHeight);
            y -= kRowSpacing;
        }

        // Show all databases (not for Redis)
        if (type != DatabaseType::REDIS) {
            self.showAllDbsCheckbox.hidden = NO;
            y -= kRowHeight;
            self.showAllDbsCheckbox.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
            y -= kRowSpacing;
        }
    }

    // Status label
    y -= 20;
    self.statusLabel.frame = NSMakeRect(kMargin, y, kDialogWidth - 2 * kMargin - 24, 20);
    self.spinner.frame = NSMakeRect(kDialogWidth - kMargin - 20, y + 2, 16, 16);
    y -= kRowSpacing;

    // Bottom separator
    y -= 1;
    self.bottomSeparator.frame = NSMakeRect(kMargin, y, kDialogWidth - 2 * kMargin, 1);
    y -= kRowSpacing;

    // Buttons
    y -= kRowHeight;
    CGFloat btnW = 90;
    self.connectButton.frame = NSMakeRect(kDialogWidth - kMargin - btnW, y, btnW, kRowHeight);
    self.cancelButton.frame =
        NSMakeRect(kDialogWidth - kMargin - btnW - 10 - btnW, y, btnW, kRowHeight);
}

// MARK: - Actions

- (void)typeChanged:(id)sender {
    DatabaseType type = [self selectedDatabaseType];

    // Update default port
    switch (type) {
    case DatabaseType::SQLITE:
        break;
    case DatabaseType::POSTGRESQL:
        self.portField.stringValue = @"5432";
        self.authSegment.selectedSegment = 0; // Default auth on
        break;
    case DatabaseType::MYSQL:
        self.portField.stringValue = @"3306";
        self.authSegment.selectedSegment = 0;
        break;
    case DatabaseType::MONGODB:
        self.portField.stringValue = @"27017";
        self.authSegment.selectedSegment = 1; // Default no auth
        break;
    case DatabaseType::REDIS:
        self.portField.stringValue = @"6379";
        self.authSegment.selectedSegment = 1;
        break;
    case DatabaseType::MARIADB:
        self.portField.stringValue = @"3306";
        self.authSegment.selectedSegment = 0;
        break;
    }

    // Clear status
    self.statusLabel.stringValue = @"";
    [self layoutFields];
}

- (void)authChanged:(id)sender {
    self.statusLabel.stringValue = @"";
    [self layoutFields];
}

- (void)browseClicked:(id)sender {
    @try {
        auto db = FileDialog::openSQLiteFile();
        if (db) {
            auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(db);
            if (sqliteDb) {
                self.sqlitePathField.stringValue =
                    [NSString stringWithUTF8String:sqliteDb->getPath().c_str()];

                NSString* currentName = self.nameField.stringValue;
                if (currentName.length == 0 ||
                    [currentName isEqualToString:@"Untitled connection"]) {
                    self.nameField.stringValue =
                        [NSString stringWithUTF8String:sqliteDb->getConnectionInfo().name.c_str()];
                }
            }
        }
    } @catch (NSException* exception) {
        NSLog(@"Exception in browseClicked: %@", exception);
    }
}

- (void)cancelClicked:(id)sender {
    [self.dialogWindow close];
}

- (void)connectClicked:(id)sender {
    @try {
        // Validate name
        NSString* nameNS = self.nameField.stringValue;
        if (nameNS.length == 0) {
            self.statusLabel.stringValue = @"Please enter a connection name";
            self.statusLabel.textColor = [NSColor systemRedColor];
            return;
        }

        DatabaseType type = [self selectedDatabaseType];

        // SQLite: synchronous
        if (type == DatabaseType::SQLITE) {
            [self connectSQLite];
            return;
        }

        // Server databases: async
        [self connectServerAsync];
    } @catch (NSException* exception) {
        NSLog(@"Exception in connectClicked: %@", exception);
        self.statusLabel.stringValue = [NSString stringWithFormat:@"Error: %@", exception.reason];
        self.statusLabel.textColor = [NSColor systemRedColor];
    }
}

- (void)connectSQLite {
    std::string sqlitePath = [self.sqlitePathField.stringValue UTF8String];
    if (sqlitePath.empty()) {
        self.statusLabel.stringValue = @"Please select a database file";
        self.statusLabel.textColor = [NSColor systemRedColor];
        return;
    }

    std::string name = [self.nameField.stringValue UTF8String];

    DatabaseConnectionInfo connInfo;
    connInfo.type = DatabaseType::SQLITE;
    connInfo.name = name;
    connInfo.path = sqlitePath;

    auto db = std::make_shared<SQLiteDatabase>(connInfo);
    auto [success, error] = db->connect();

    if (success) {
        [self handleSuccess:db info:connInfo];
        [self.dialogWindow close];
    } else {
        self.statusLabel.stringValue = [NSString stringWithUTF8String:("Failed: " + error).c_str()];
        self.statusLabel.textColor = [NSColor systemRedColor];
    }
}

- (void)connectServerAsync {
    // Capture all field values
    std::string name = [self.nameField.stringValue UTF8String];
    DatabaseType type = [self selectedDatabaseType];
    std::string host = [self.hostField.stringValue UTF8String];
    int port = [self.portField.stringValue intValue];
    if (port <= 0)
        port = 1;
    if (port > 65535)
        port = 65535;
    std::string database = [self.databaseField.stringValue UTF8String];
    bool authEnabled = (self.authSegment.selectedSegment == 0);
    std::string username =
        authEnabled ? std::string([self.usernameField.stringValue UTF8String]) : "";
    std::string password =
        authEnabled ? std::string([self.passwordField.stringValue UTF8String]) : "";
    bool showAllDbs = (self.showAllDbsCheckbox.state == NSControlStateValueOn);
    int sslModeIdx = (int)[self.sslModePopup indexOfSelectedItem];
    static const char* sslModes[] = {"disable", "allow",     "prefer",
                                     "require", "verify-ca", "verify-full"};

    Application* appPtr = self.app;
    int editConnId = self.editingConnectionId;
    std::shared_ptr<DatabaseInterface> editingDbCopy = _editingDb;

    // Validate
    if (authEnabled && username.empty() && type != DatabaseType::MONGODB &&
        type != DatabaseType::REDIS) {
        self.statusLabel.stringValue = @"Please enter a username";
        self.statusLabel.textColor = [NSColor systemRedColor];
        return;
    }

    // UI feedback
    self.connectButton.enabled = NO;
    [self.spinner startAnimation:nil];
    self.statusLabel.stringValue = @"Connecting...";
    self.statusLabel.textColor = [NSColor secondaryLabelColor];

    // Retain UI objects for async callback
    NSWindow* dialogRef = self.dialogWindow;
    NSButton* connectBtnRef = self.connectButton;
    NSTextField* statusRef = self.statusLabel;
    NSProgressIndicator* spinnerRef = self.spinner;
    std::atomic<bool>* cancelledFlag = &_cancelled;
    [dialogRef retain];
    [connectBtnRef retain];
    [statusRef retain];
    [spinnerRef retain];

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
      // Build connection info
      DatabaseConnectionInfo info;
      info.type = type;
      info.name = name;
      info.host = host;
      info.port = port;
      info.showAllDatabases = showAllDbs;
      if (authEnabled) {
          info.username = username;
          info.password = password;
      }

      std::shared_ptr<DatabaseInterface> db;

      switch (type) {
      case DatabaseType::POSTGRESQL:
          info.database = database.empty() ? "postgres" : database;
          info.sslmode = sslModes[sslModeIdx];
          db = std::make_shared<PostgresDatabase>(info);
          break;
      case DatabaseType::MYSQL:
      case DatabaseType::MARIADB:
          info.database = database.empty() ? "mysql" : database;
          db = std::make_shared<MySQLDatabase>(info);
          break;
      case DatabaseType::MONGODB:
          info.database = database;
          db = std::make_shared<MongoDBDatabase>(info);
          break;
      case DatabaseType::REDIS:
          db = std::make_shared<RedisDatabase>(info);
          break;
      default:
          break;
      }

      bool success = false;
      std::string errorMsg;

      if (db) {
          auto [s, e] = db->connect();
          success = s;
          errorMsg = e;
      } else {
          errorMsg = "Failed to create database connection";
      }

      // Capture for main thread
      auto dbResult = db;
      auto infoResult = info;

      dispatch_async(dispatch_get_main_queue(), ^{
        if (cancelledFlag->load()) {
            // Dialog was closed/cancelled — discard the connection
            if (success && dbResult) {
                dbResult->disconnect();
            }
        } else if (success) {
            // Handle success
            if (editConnId != -1 && editingDbCopy) {
                // Edit mode: update saved connection and replace database
                SavedConnection conn;
                conn.id = editConnId;
                conn.connectionInfo = infoResult;
                conn.workspaceId = appPtr->getCurrentWorkspaceId();
                appPtr->getAppState()->updateConnection(conn);

                dbResult->setConnectionId(editConnId);
                auto& dbs = appPtr->getDatabases();
                for (size_t i = 0; i < dbs.size(); i++) {
                    if (dbs[i] == editingDbCopy) {
                        dbs[i]->disconnect();
                        dbs[i] = dbResult;
                        break;
                    }
                }
            } else {
                // New connection: save and add
                SavedConnection conn;
                conn.connectionInfo = infoResult;
                conn.workspaceId = appPtr->getCurrentWorkspaceId();
                int newId = appPtr->getAppState()->saveConnection(conn);
                if (newId != -1) {
                    dbResult->setConnectionId(newId);
                }
                appPtr->addDatabase(dbResult);
            }
            [dialogRef close];
        } else {
            NSString* errStr = [NSString stringWithUTF8String:("Failed: " + errorMsg).c_str()];
            statusRef.stringValue = errStr;
            statusRef.textColor = [NSColor systemRedColor];
            connectBtnRef.enabled = YES;
            [spinnerRef stopAnimation:nil];
        }

        [dialogRef release];
        [connectBtnRef release];
        [statusRef release];
        [spinnerRef release];
      });
    });
}

- (void)handleSuccess:(std::shared_ptr<DatabaseInterface>)db
                 info:(const DatabaseConnectionInfo&)info {
    Application* appPtr = self.app;
    if (!appPtr)
        return;

    if (self.editingConnectionId != -1 && _editingDb) {
        // Edit mode
        SavedConnection conn;
        conn.id = self.editingConnectionId;
        conn.connectionInfo = info;
        conn.workspaceId = appPtr->getCurrentWorkspaceId();
        appPtr->getAppState()->updateConnection(conn);

        db->setConnectionId(self.editingConnectionId);
        auto& dbs = appPtr->getDatabases();
        for (size_t i = 0; i < dbs.size(); i++) {
            if (dbs[i] == _editingDb) {
                dbs[i]->disconnect();
                dbs[i] = db;
                break;
            }
        }
    } else {
        // New connection
        SavedConnection conn;
        conn.connectionInfo = info;
        conn.workspaceId = appPtr->getCurrentWorkspaceId();
        int newId = appPtr->getAppState()->saveConnection(conn);
        if (newId != -1) {
            db->setConnectionId(newId);
        }
        appPtr->addDatabase(db);
    }
}

// MARK: - NSWindowDelegate

- (void)windowWillClose:(NSNotification*)notification {
    _cancelled = true;

    // Refocus main app window
    if (self.app) {
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            NSWindow* mainWindow = glfwGetCocoaWindow(glfwWindow);
            [mainWindow makeKeyAndOrderFront:nil];
        }
    }

    _editingDb.reset();
    sActiveConnectionDialog = nil;
    // The associated object retains us; clearing it triggers our dealloc
    objc_setAssociatedObject(self.dialogWindow, "controller", nil, OBJC_ASSOCIATION_RETAIN);
}

@end

// MARK: - CreateDatabaseDialogController

@interface CreateDatabaseDialogController : NSObject <NSWindowDelegate> {
    std::shared_ptr<DatabaseInterface> _db;
}

@property(nonatomic, assign) Application* app;
@property(nonatomic, strong) NSWindow* dialogWindow;

// Common fields
@property(nonatomic, strong) NSTextField* nameLabel;
@property(nonatomic, strong) NSTextField* nameField;
@property(nonatomic, strong) NSTextField* commentLabel;
@property(nonatomic, strong) NSTextField* commentField;

// PostgreSQL fields
@property(nonatomic, strong) NSTextField* ownerLabel;
@property(nonatomic, strong) NSPopUpButton* ownerPopup;
@property(nonatomic, strong) NSTextField* templateLabel;
@property(nonatomic, strong) NSPopUpButton* templatePopup;
@property(nonatomic, strong) NSTextField* encodingLabel;
@property(nonatomic, strong) NSPopUpButton* encodingPopup;
@property(nonatomic, strong) NSTextField* tablespaceLabel;
@property(nonatomic, strong) NSPopUpButton* tablespacePopup;

// MySQL fields
@property(nonatomic, strong) NSTextField* charsetLabel;
@property(nonatomic, strong) NSPopUpButton* charsetPopup;
@property(nonatomic, strong) NSTextField* collationLabel;
@property(nonatomic, strong) NSPopUpButton* collationPopup;

// Bottom controls
@property(nonatomic, strong) NSBox* bottomSeparator;
@property(nonatomic, strong) NSTextField* statusLabel;
@property(nonatomic, strong) NSProgressIndicator* spinner;
@property(nonatomic, strong) NSButton* createButton;
@property(nonatomic, strong) NSButton* cancelButton;

- (void)showDialogForDatabase:(std::shared_ptr<DatabaseInterface>)db;

@end

static NSWindow* sActiveCreateDatabaseDialog = nil;

@implementation CreateDatabaseDialogController

- (instancetype)init {
    self = [super init];
    return self;
}

- (void)dealloc {
    _db.reset();
    [super dealloc];
}

- (NSTextField*)makeLabel:(NSString*)text {
    NSTextField* label = [NSTextField labelWithString:text];
    label.alignment = NSTextAlignmentRight;
    label.textColor = [NSColor secondaryLabelColor];
    label.font = [NSFont systemFontOfSize:13];
    return label;
}

- (NSTextField*)makeTextField:(NSString*)placeholder {
    NSTextField* field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.bezeled = YES;
    field.bezelStyle = NSTextFieldRoundedBezel;
    field.editable = YES;
    field.selectable = YES;
    return field;
}

- (NSPopUpButton*)makePopup:(NSArray<NSString*>*)items defaultIndex:(NSInteger)defaultIdx {
    NSPopUpButton* popup = [[NSPopUpButton alloc] init];
    for (NSString* item in items) {
        [popup addItemWithTitle:item];
    }
    if (defaultIdx >= 0 && defaultIdx < (NSInteger)items.count) {
        [popup selectItemAtIndex:defaultIdx];
    }
    return popup;
}

- (void)ensureEditMenu {
    NSMenu* mainMenu = [NSApp mainMenu];
    if (!mainMenu) {
        mainMenu = [[NSMenu alloc] init];
        [NSApp setMainMenu:mainMenu];
    }

    for (NSMenuItem* item in mainMenu.itemArray) {
        if ([item.title isEqualToString:@"Edit"])
            return;
    }

    NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
    editMenuItem.title = @"Edit";
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];

    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                                 action:@selector(undo:)
                                          keyEquivalent:@"z"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Redo"
                                                 action:@selector(redo:)
                                          keyEquivalent:@"Z"]];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Cut"
                                                 action:@selector(cut:)
                                          keyEquivalent:@"x"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                 action:@selector(copy:)
                                          keyEquivalent:@"c"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                 action:@selector(paste:)
                                          keyEquivalent:@"v"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                 action:@selector(selectAll:)
                                          keyEquivalent:@"a"]];

    editMenuItem.submenu = editMenu;
    [mainMenu addItem:editMenuItem];
}

- (void)showDialogForDatabase:(std::shared_ptr<DatabaseInterface>)db {
    _db = db;
    [self ensureEditMenu];
    [self buildControls];
    [self layoutFields];

    // Center on main window
    NSWindow* mainWindow = nil;
    if (self.app) {
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            mainWindow = glfwGetCocoaWindow(glfwWindow);
        }
    }

    if (mainWindow) {
        [self.dialogWindow setLevel:NSModalPanelWindowLevel];
        NSRect mainFrame = mainWindow.frame;
        NSRect dialogFrame = self.dialogWindow.frame;
        CGFloat x = NSMidX(mainFrame) - dialogFrame.size.width / 2;
        CGFloat y = NSMidY(mainFrame) - dialogFrame.size.height / 2;
        [self.dialogWindow setFrameOrigin:NSMakePoint(x, y)];
    }

    // Match app theme
    if (self.app) {
        NSAppearanceName appearanceName =
            self.app->isDarkTheme() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
        self.dialogWindow.appearance = [NSAppearance appearanceNamed:appearanceName];
    }

    [self.dialogWindow makeKeyAndOrderFront:nil];
}

- (DatabaseType)databaseType {
    return _db ? _db->getConnectionInfo().type : DatabaseType::SQLITE;
}

- (void)buildControls {
    DatabaseType type = [self databaseType];
    bool isPostgres = (type == DatabaseType::POSTGRESQL);

    self.dialogWindow =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kDialogWidth, 320)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskFullSizeContentView
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.dialogWindow.titlebarAppearsTransparent = YES;
    self.dialogWindow.titleVisibility = NSWindowTitleHidden;
    [self.dialogWindow standardWindowButton:NSWindowMiniaturizeButton].hidden = YES;
    [self.dialogWindow standardWindowButton:NSWindowZoomButton].hidden = YES;
    self.dialogWindow.delegate = self;

    objc_setAssociatedObject(self.dialogWindow, "controller", self, OBJC_ASSOCIATION_RETAIN);

    NSView* cv = self.dialogWindow.contentView;

    // Name
    self.nameLabel = [self makeLabel:@"Name"];
    [cv addSubview:self.nameLabel];
    self.nameField = [self makeTextField:@"new_database"];
    [cv addSubview:self.nameField];

    if (isPostgres) {
        // Owner
        self.ownerLabel = [self makeLabel:@"Owner"];
        [cv addSubview:self.ownerLabel];
        self.ownerPopup = [self makePopup:@[ @"postgres" ] defaultIndex:0];
        [cv addSubview:self.ownerPopup];

        // Template
        self.templateLabel = [self makeLabel:@"Template"];
        [cv addSubview:self.templateLabel];
        self.templatePopup = [self makePopup:@[ @"template1", @"template0" ] defaultIndex:0];
        [cv addSubview:self.templatePopup];

        // Encoding
        self.encodingLabel = [self makeLabel:@"Encoding"];
        [cv addSubview:self.encodingLabel];
        self.encodingPopup = [self makePopup:@[
            @"UTF8", @"LATIN1", @"LATIN2", @"LATIN9", @"WIN1252", @"SQL_ASCII", @"EUC_JP",
            @"EUC_KR", @"EUC_CN", @"SJIS", @"BIG5", @"WIN1251", @"ISO_8859_5"
        ]
                                defaultIndex:0];
        [cv addSubview:self.encodingPopup];

        // Tablespace
        self.tablespaceLabel = [self makeLabel:@"Tablespace"];
        [cv addSubview:self.tablespaceLabel];
        self.tablespacePopup = [self makePopup:@[ @"pg_default" ] defaultIndex:0];
        [cv addSubview:self.tablespacePopup];

        // Populate dynamic options from database
        [self populatePostgresOptions];
    } else {
        // MySQL: Charset
        self.charsetLabel = [self makeLabel:@"Charset"];
        [cv addSubview:self.charsetLabel];
        self.charsetPopup = [self makePopup:@[
            @"utf8mb4", @"utf8mb3", @"utf8", @"latin1", @"ascii", @"binary", @"utf16", @"utf32",
            @"cp1251", @"gbk", @"big5", @"euckr", @"sjis"
        ]
                               defaultIndex:0];
        [self.charsetPopup setTarget:self];
        [self.charsetPopup setAction:@selector(charsetChanged:)];
        [cv addSubview:self.charsetPopup];

        // Collation
        self.collationLabel = [self makeLabel:@"Collation"];
        [cv addSubview:self.collationLabel];
        self.collationPopup = [self makePopup:@[
            @"utf8mb4_unicode_ci", @"utf8mb4_0900_ai_ci", @"utf8mb4_general_ci", @"utf8mb4_bin"
        ]
                                 defaultIndex:0];
        [cv addSubview:self.collationPopup];
    }

    // Comment
    self.commentLabel = [self makeLabel:@"Comment"];
    [cv addSubview:self.commentLabel];
    self.commentField = [self makeTextField:@"(optional)"];
    [cv addSubview:self.commentField];

    // Bottom separator
    self.bottomSeparator = [[NSBox alloc] init];
    self.bottomSeparator.boxType = NSBoxSeparator;
    [cv addSubview:self.bottomSeparator];

    // Status label
    self.statusLabel = [NSTextField labelWithString:@""];
    self.statusLabel.textColor = [NSColor systemRedColor];
    [cv addSubview:self.statusLabel];

    // Spinner
    self.spinner = [[NSProgressIndicator alloc] init];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.displayedWhenStopped = NO;
    [cv addSubview:self.spinner];

    // Create button
    self.createButton = [[NSButton alloc] init];
    [self.createButton setTitle:@"Create"];
    [self.createButton setBezelStyle:NSBezelStyleRounded];
    [self.createButton setKeyEquivalent:@"\r"];
    [self.createButton setTarget:self];
    [self.createButton setAction:@selector(createClicked:)];
    [cv addSubview:self.createButton];

    // Cancel button
    self.cancelButton = [[NSButton alloc] init];
    [self.cancelButton setTitle:@"Cancel"];
    [self.cancelButton setBezelStyle:NSBezelStyleRounded];
    [self.cancelButton setKeyEquivalent:@"\033"];
    [self.cancelButton setTarget:self];
    [self.cancelButton setAction:@selector(cancelClicked:)];
    [cv addSubview:self.cancelButton];
}

- (void)populatePostgresOptions {
    if (!_db)
        return;

    auto* executor = dynamic_cast<IQueryExecutor*>(_db.get());
    if (!executor)
        return;

    // Populate owners from pg_roles
    @try {
        auto result = executor->executeQuery("SELECT rolname FROM pg_roles ORDER BY rolname");
        if (result.success()) {
            [self.ownerPopup removeAllItems];
            NSInteger postgresIdx = -1;
            for (const auto& row : result[0].tableData) {
                if (!row.empty()) {
                    NSString* name = [NSString stringWithUTF8String:row[0].c_str()];
                    [self.ownerPopup addItemWithTitle:name];
                    if ([name isEqualToString:@"postgres"]) {
                        postgresIdx = self.ownerPopup.numberOfItems - 1;
                    }
                }
            }
            if (postgresIdx >= 0) {
                [self.ownerPopup selectItemAtIndex:postgresIdx];
            }
        }
    } @catch (NSException* e) {
        NSLog(@"Failed to load pg_roles: %@", e);
    }

    // Populate templates from pg_database
    @
    try {
        auto result = executor->executeQuery(
            "SELECT datname FROM pg_database WHERE datistemplate ORDER BY datname");
        if (result.success()) {
            [self.templatePopup removeAllItems];
            [self.templatePopup addItemWithTitle:@"template1"];
            for (const auto& row : result[0].tableData) {
                if (!row.empty()) {
                    NSString* name = [NSString stringWithUTF8String:row[0].c_str()];
                    if (![name isEqualToString:@"template1"]) {
                        [self.templatePopup addItemWithTitle:name];
                    }
                }
            }
        }
    } @catch (NSException* e) {
        NSLog(@"Failed to load template databases: %@", e);
    }

    // Populate tablespaces from pg_tablespace
    @
    try {
        auto result = executor->executeQuery("SELECT spcname FROM pg_tablespace ORDER BY spcname");
        if (result.success()) {
            [self.tablespacePopup removeAllItems];
            for (const auto& row : result[0].tableData) {
                if (!row.empty()) {
                    [self.tablespacePopup
                        addItemWithTitle:[NSString stringWithUTF8String:row[0].c_str()]];
                }
            }
        }
    } @catch (NSException* e) {
        NSLog(@"Failed to load tablespaces: %@", e);
    }
}

- (void)charsetChanged:(id)sender {
    NSString* charset = [self.charsetPopup titleOfSelectedItem];
    [self.collationPopup removeAllItems];

    if ([charset isEqualToString:@"utf8mb4"]) {
        for (NSString* c in @[
                 @"utf8mb4_unicode_ci", @"utf8mb4_0900_ai_ci", @"utf8mb4_general_ci", @"utf8mb4_bin"
             ]) {
            [self.collationPopup addItemWithTitle:c];
        }
    } else if ([charset isEqualToString:@"utf8mb3"] || [charset isEqualToString:@"utf8"]) {
        for (NSString* c in @[ @"utf8_unicode_ci", @"utf8_general_ci", @"utf8_bin" ]) {
            [self.collationPopup addItemWithTitle:c];
        }
    } else if ([charset isEqualToString:@"latin1"]) {
        for (NSString* c in @[ @"latin1_swedish_ci", @"latin1_general_ci", @"latin1_bin" ]) {
            [self.collationPopup addItemWithTitle:c];
        }
    } else if ([charset isEqualToString:@"ascii"]) {
        for (NSString* c in @[ @"ascii_general_ci", @"ascii_bin" ]) {
            [self.collationPopup addItemWithTitle:c];
        }
    } else if ([charset isEqualToString:@"binary"]) {
        [self.collationPopup addItemWithTitle:@"binary"];
    }
}

- (CGFloat)computeRequiredHeight {
    CGFloat h = kMargin;
    h += kRowHeight + kRowSpacing; // Name

    DatabaseType type = [self databaseType];
    if (type == DatabaseType::POSTGRESQL) {
        h += kRowHeight + kRowSpacing; // Owner
        h += kRowHeight + kRowSpacing; // Template
        h += kRowHeight + kRowSpacing; // Encoding
        h += kRowHeight + kRowSpacing; // Tablespace
    } else {
        h += kRowHeight + kRowSpacing; // Charset
        h += kRowHeight + kRowSpacing; // Collation
    }

    h += kRowHeight + kRowSpacing; // Comment
    h += 20 + kRowSpacing;         // Status
    h += 1 + kRowSpacing;          // Separator
    h += kRowHeight;               // Buttons
    h += kMargin;
    return h;
}

- (void)layoutFields {
    CGFloat windowH = [self computeRequiredHeight];

    NSRect frame = self.dialogWindow.frame;
    CGFloat topEdge = NSMaxY(frame);
    frame.size.height = windowH;
    frame.origin.y = topEdge - windowH;
    NSRect contentRect = [self.dialogWindow contentRectForFrameRect:frame];
    CGFloat contentH = contentRect.size.height;
    [self.dialogWindow setFrame:frame display:YES animate:NO];

    CGFloat y = contentH - kMargin;
    DatabaseType type = [self databaseType];

    // Name row
    y -= kRowHeight;
    self.nameLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
    self.nameField.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
    y -= kRowSpacing;

    if (type == DatabaseType::POSTGRESQL) {
        // Owner
        y -= kRowHeight;
        self.ownerLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.ownerPopup.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
        y -= kRowSpacing;

        // Template
        y -= kRowHeight;
        self.templateLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.templatePopup.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
        y -= kRowSpacing;

        // Encoding
        y -= kRowHeight;
        self.encodingLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.encodingPopup.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
        y -= kRowSpacing;

        // Tablespace
        y -= kRowHeight;
        self.tablespaceLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.tablespacePopup.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
        y -= kRowSpacing;
    } else {
        // Charset
        y -= kRowHeight;
        self.charsetLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.charsetPopup.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
        y -= kRowSpacing;

        // Collation
        y -= kRowHeight;
        self.collationLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
        self.collationPopup.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
        y -= kRowSpacing;
    }

    // Comment
    y -= kRowHeight;
    self.commentLabel.frame = NSMakeRect(kMargin, y, kLabelWidth, kRowHeight);
    self.commentField.frame = NSMakeRect(kFieldX, y, kFieldWidth, kRowHeight);
    y -= kRowSpacing;

    // Status label
    y -= 20;
    self.statusLabel.frame = NSMakeRect(kMargin, y, kDialogWidth - 2 * kMargin - 24, 20);
    self.spinner.frame = NSMakeRect(kDialogWidth - kMargin - 20, y + 2, 16, 16);
    y -= kRowSpacing;

    // Bottom separator
    y -= 1;
    self.bottomSeparator.frame = NSMakeRect(kMargin, y, kDialogWidth - 2 * kMargin, 1);
    y -= kRowSpacing;

    // Buttons
    y -= kRowHeight;
    CGFloat btnW = 90;
    self.createButton.frame = NSMakeRect(kDialogWidth - kMargin - btnW, y, btnW, kRowHeight);
    self.cancelButton.frame =
        NSMakeRect(kDialogWidth - kMargin - btnW - 10 - btnW, y, btnW, kRowHeight);
}

- (void)cancelClicked:(id)sender {
    [self.dialogWindow close];
}

- (void)createClicked:(id)sender {
    @try {
        NSString* nameNS = self.nameField.stringValue;
        if (nameNS.length == 0) {
            self.statusLabel.stringValue = @"Please enter a database name";
            self.statusLabel.textColor = [NSColor systemRedColor];
            return;
        }

        self.createButton.enabled = NO;
        [self.spinner startAnimation:nil];
        self.statusLabel.stringValue = @"Creating...";
        self.statusLabel.textColor = [NSColor secondaryLabelColor];

        // Build options
        CreateDatabaseOptions opts;
        opts.name = [nameNS UTF8String];
        opts.comment = [self.commentField.stringValue UTF8String];

        DatabaseType type = [self databaseType];
        if (type == DatabaseType::POSTGRESQL) {
            opts.owner = [[self.ownerPopup titleOfSelectedItem] UTF8String];
            opts.templateDb = [[self.templatePopup titleOfSelectedItem] UTF8String];
            opts.encoding = [[self.encodingPopup titleOfSelectedItem] UTF8String];
            opts.tablespace = [[self.tablespacePopup titleOfSelectedItem] UTF8String];
        } else {
            opts.charset = [[self.charsetPopup titleOfSelectedItem] UTF8String];
            opts.collation = [[self.collationPopup titleOfSelectedItem] UTF8String];
        }

        // Capture for async
        std::shared_ptr<DatabaseInterface> dbCopy = _db;
        Application* appPtr = self.app;
        NSWindow* dialogRef = self.dialogWindow;
        NSButton* createBtnRef = self.createButton;
        NSTextField* statusRef = self.statusLabel;
        NSProgressIndicator* spinnerRef = self.spinner;
        [dialogRef retain];
        [createBtnRef retain];
        [statusRef retain];
        [spinnerRef retain];

        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
          auto result = dbCopy->createDatabaseWithOptions(opts);
          bool ok = result.first;
          std::string errMsg = result.second;

          dispatch_async(dispatch_get_main_queue(), ^{
            if (ok) {
                // Refresh the database list
                if (auto* pgDb = dynamic_cast<PostgresDatabase*>(dbCopy.get())) {
                    pgDb->refreshDatabaseNames();
                } else if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(dbCopy.get())) {
                    mysqlDb->refreshDatabaseNames();
                }
                [dialogRef close];
            } else {
                NSString* errStr = [NSString stringWithUTF8String:("Failed: " + errMsg).c_str()];
                statusRef.stringValue = errStr;
                statusRef.textColor = [NSColor systemRedColor];
                createBtnRef.enabled = YES;
                [spinnerRef stopAnimation:nil];
            }

            [dialogRef release];
            [createBtnRef release];
            [statusRef release];
            [spinnerRef release];
          });
        });
    } @catch (NSException* exception) {
        NSLog(@"Exception in createClicked: %@", exception);
        self.statusLabel.stringValue = [NSString stringWithFormat:@"Error: %@", exception.reason];
        self.statusLabel.textColor = [NSColor systemRedColor];
        self.createButton.enabled = YES;
        [self.spinner stopAnimation:nil];
    }
}

- (void)windowWillClose:(NSNotification*)notification {
    if (self.app) {
        GLFWwindow* glfwWindow = self.app->getWindow();
        if (glfwWindow) {
            NSWindow* mainWindow = glfwGetCocoaWindow(glfwWindow);
            [mainWindow makeKeyAndOrderFront:nil];
        }
    }

    _db.reset();
    sActiveCreateDatabaseDialog = nil;
    objc_setAssociatedObject(self.dialogWindow, "controller", nil, OBJC_ASSOCIATION_RETAIN);
}

@end

// MARK: - C++ free functions

void showMacOSConnectionDialog(Application* app) {
    if (sActiveConnectionDialog) {
        [sActiveConnectionDialog makeKeyAndOrderFront:nil];
        return;
    }
    ConnectionDialogController* controller = [[ConnectionDialogController alloc] init];
    controller.app = app;
    [controller showDialog];
    sActiveConnectionDialog = controller.dialogWindow;
    [controller release]; // associated object on the window holds the retain
}

void showMacOSEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db,
                                   int connectionId) {
    if (sActiveConnectionDialog) {
        [sActiveConnectionDialog makeKeyAndOrderFront:nil];
        return;
    }
    ConnectionDialogController* controller = [[ConnectionDialogController alloc] init];
    controller.app = app;
    [controller showDialogForEdit:db connectionId:connectionId];
    sActiveConnectionDialog = controller.dialogWindow;
    [controller release]; // associated object on the window holds the retain
}

void showMacOSCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db) {
    if (sActiveCreateDatabaseDialog) {
        [sActiveCreateDatabaseDialog makeKeyAndOrderFront:nil];
        return;
    }
    CreateDatabaseDialogController* controller = [[CreateDatabaseDialogController alloc] init];
    controller.app = app;
    [controller showDialogForDatabase:db];
    sActiveCreateDatabaseDialog = controller.dialogWindow;
    [controller release]; // associated object on the window holds the retain
}
