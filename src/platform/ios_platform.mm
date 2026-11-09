#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include "application.hpp"
#include "database/connection_url.hpp"
#include "database/file_database.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "mydear/ios.h"
#include "platform/ios_platform.hpp"
#include "themes.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab_manager.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

// ---- the root controller: mydear's imgui view, plus the navigation bar ----

namespace {
    UINavigationController* gNav = nil; // the bar the theme paints
}

@interface DearSQLViewController : MydearViewController
@end

@implementation DearSQLViewController {
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
    const bool compact = self.view.bounds.size.width < 700;
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
    [super loadView];
    gNav = self.navigationController; // initialize() applies the theme to it
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
- (void)viewDidLoad {
    [super viewDidLoad];
    self.navigationController.navigationBarHidden = NO;
    [self setupBar];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // ponytail: screenshots in landscape, as simctl cannot rotate a simulator
    if (getenv("DEARSQL_LANDSCAPE"))
        [self.view.window.windowScene
            requestGeometryUpdateWithPreferences:
                [[UIWindowSceneGeometryPreferencesIOS alloc]
                    initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscapeRight]
                                    errorHandler:nil];
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        // ~/.dearsql lands in the container's Library: its root is read-only on a device
        setenv("HOME", [NSHomeDirectory() stringByAppendingPathComponent:@"Library"].UTF8String, 1);
        setenv("DEARSQL_CONTAINER", NSHomeDirectory().UTF8String, 1); // saved paths re-root here
        return mydear::RunUIKit(argc, argv, DearSQLViewController.class);
    }
}

// ---- IOSPlatform ----

IOSPlatform::IOSPlatform(Application* app, mydear::App& shell) : app_(app), shell_(shell) {
    // the file picker gives the keyboard back to the imgui view by this tag
    ((__bridge UIView*)shell_.nativeView()).tag = kDearSQLViewTag;
}

void IOSPlatform::onSidebarToggleClicked() {
    app_->setSidebarVisible(!app_->isSidebarVisible());
}

float IOSPlatform::getClientAreaTopInset() const {
    UIView* view = (__bridge UIView*)shell_.nativeView();
    return view ? static_cast<float>(view.safeAreaInsets.top) : 0.0f;
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
    shell_.renderFrame();
}

void IOSPlatform::renderChrome() {
    showRequestedTabs();
}

ImTextureID IOSPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    return shell_.createTexture(pixels, width, height);
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
