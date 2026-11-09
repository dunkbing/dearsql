#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "platform/alert.hpp"
#include "platform/updater.hpp"
#include "ui/input_dialog.hpp"
#include "utils/cli_command.hpp"
#include "utils/file_dialog.hpp"

#include <filesystem>
#include <memory>

namespace {
    NSString* ns(const std::string& s) {
        return [NSString stringWithUTF8String:s.c_str()];
    }

    UIViewController* topController() {
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class])
                continue;
            UIViewController* top = ((UIWindowScene*)scene).keyWindow.rootViewController;
            while (top.presentedViewController)
                top = top.presentedViewController;
            if (top)
                return top;
        }
        return nil;
    }

    UIAlertActionStyle styleOf(AlertButton::Style style) {
        switch (style) {
        case AlertButton::Style::Cancel:
            return UIAlertActionStyleCancel;
        case AlertButton::Style::Destructive:
            return UIAlertActionStyleDestructive;
        default:
            return UIAlertActionStyleDefault;
        }
    }
} // namespace

// alerts don't block on ipad: the button's callback runs later, on the main thread
// (between frames, like everything else in the app)
void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty())
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:ns(title)
                                            message:message.empty() ? nil : ns(message)
                                     preferredStyle:UIAlertControllerStyleAlert];
    bool hasCancel = false;
    for (auto& button : buttons) {
        // ios allows one cancel action per alert
        auto style = styleOf(button.style);
        if (style == UIAlertActionStyleCancel && std::exchange(hasCancel, true))
            style = UIAlertActionStyleDefault;
        auto onPress = button.onPress;
        [alert addAction:[UIAlertAction actionWithTitle:ns(button.text)
                                                  style:style
                                                handler:^(UIAlertAction*) {
                                                  if (onPress)
                                                      onPress();
                                                }]];
    }
    [topController() presentViewController:alert animated:YES completion:nil];
}

void InputDialog::show(const std::string& title, const std::string& label,
                       const std::string& initialValue, const std::string& confirmButtonText,
                       ConfirmCallback onConfirm, CancelCallback onCancel,
                       ValidatorCallback validator) {
    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:ns(title)
                                            message:label.empty() ? nil : ns(label)
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
      field.text = ns(initialValue);
      field.autocapitalizationType = UITextAutocapitalizationTypeNone;
      field.autocorrectionType = UITextAutocorrectionTypeNo;
    }];
    __weak UIAlertController* weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:^(UIAlertAction*) {
                                              if (onCancel)
                                                  onCancel();
                                            }]];
    [alert addAction:[UIAlertAction
                         actionWithTitle:ns(confirmButtonText)
                                   style:UIAlertActionStyleDefault
                                 handler:^(UIAlertAction*) {
                                   const std::string value =
                                       weakAlert.textFields.firstObject.text.UTF8String ?: "";
                                   std::string error = validator ? validator(value) : "";
                                   if (error.empty() && onConfirm)
                                       error = onConfirm(value);
                                   // an alert closes on any button: ask again, with the error
                                   if (!error.empty())
                                       InputDialog::show(title, error, value, confirmButtonText,
                                                         onConfirm, onCancel, validator);
                                 }]];
    [topController() presentViewController:alert animated:YES completion:nil];
}

// the app store updates the app
void initializeUpdater() {}
void checkForUpdates() {}
void pollUpdater() {}
void cleanupUpdater() {}
bool isUpdateAvailable() {
    return false;
}
std::string getLatestVersion() {
    return {};
}

// no terminal on ipad
namespace CliCommand {
    bool supported() {
        return false;
    }
    std::string executable() {
        return {};
    }
    std::string linkPath() {
        return {};
    }
    bool installed() {
        return false;
    }
    std::string install() {
        return "Not available on iPad.";
    }
} // namespace CliCommand

// ---- file dialogs: the document picker ----

@interface DearSQLPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) BOOL done;
@property(nonatomic, strong) NSURL* url;
@end

@implementation DearSQLPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)picker
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    self.url = urls.firstObject;
    self.done = YES;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)picker {
    self.done = YES;
}
@end

namespace {
    NSURL* directory(NSSearchPathDirectory which, NSString* sub) {
        NSURL* url = [NSFileManager.defaultManager URLsForDirectory:which
                                                          inDomains:NSUserDomainMask]
                         .firstObject;
        if (sub)
            url = [url URLByAppendingPathComponent:sub isDirectory:YES];
        [NSFileManager.defaultManager createDirectoryAtURL:url
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];
        return url;
    }

    // what the files app shows under "DearSQL": databases, csv files, exports
    NSURL* documents() {
        return directory(NSDocumentDirectory, nil);
    }

    // keys, certificates, uploads: kept out of the files app
    NSURL* imported() {
        return directory(NSApplicationSupportDirectory, @"Imported");
    }

    // "name.ext", or "name 2.ext" when taken
    NSURL* freeURL(NSURL* dir, NSString* fileName) {
        NSURL* url = [dir URLByAppendingPathComponent:fileName];
        NSString* stem = fileName.stringByDeletingPathExtension;
        NSString* ext = fileName.pathExtension;
        for (int n = 2; [NSFileManager.defaultManager fileExistsAtPath:url.path]; ++n) {
            NSString* name = [NSString stringWithFormat:@"%@ %d", stem, n];
            url = [dir URLByAppendingPathComponent:ext.length
                                                       ? [name stringByAppendingPathExtension:ext]
                                                       : name];
        }
        return url;
    }

    std::string path(NSURL* url) {
        return url ? std::string(url.path.UTF8String) : std::string();
    }

    // ponytail: a modal loop, as NSOpenPanel runs on the mac, since every caller wants the
    // path back at once. drawing pauses meanwhile (we are inside a frame)
    std::string pick(NSArray<UTType*>* types, NSURL* into) {
        UIViewController* top = topController();
        if (!top)
            return {};
        UIViewController* root = top.view.window.rootViewController;
        MTKView* view = [root.view isKindOfClass:MTKView.class] ? (MTKView*)root.view : nil;
        UIDocumentPickerViewController* picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
        DearSQLPickerDelegate* delegate = [DearSQLPickerDelegate new];
        picker.delegate = delegate;
        const BOOL wasPaused = view.paused;
        view.paused = YES;
        [top presentViewController:picker animated:YES completion:nil];
        while (!delegate.done)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        view.paused = wasPaused;
        [view becomeFirstResponder];

        NSURL* url = delegate.url;
        if (!url)
            return {};
        // a file of ours opens in place
        NSString* container = NSHomeDirectory().stringByStandardizingPath;
        if ([url.path.stringByStandardizingPath hasPrefix:container])
            return path(url);
        // anything else is copied in: sqlite needs to create its journal next to the
        // database, and access to a picked file does not cover its folder
        const BOOL scoped = [url startAccessingSecurityScopedResource];
        NSURL* copy = freeURL(into, url.lastPathComponent);
        NSError* error = nil;
        const BOOL ok = [NSFileManager.defaultManager copyItemAtURL:url toURL:copy error:&error];
        if (scoped)
            [url stopAccessingSecurityScopedResource];
        if (!ok) {
            Alert::show("Could not open the file", error.localizedDescription.UTF8String ?: "");
            return {};
        }
        return path(copy);
    }
} // namespace

bool FileDialog::isInitialized = true;

bool FileDialog::initialize() {
    return true;
}

void FileDialog::cleanup() {}

std::shared_ptr<DatabaseInterface> FileDialog::openDatabaseFile(DatabaseType type) {
    const std::string file = pick(@[ UTTypeData ], documents());
    if (file.empty())
        return nullptr;
    DatabaseConnectionInfo info;
    info.type = type;
    info.name = std::filesystem::path(file).filename().string();
    info.path = file;
    return DatabaseFactory::createDatabase(info);
}

std::string FileDialog::openFile() {
    return pick(@[ UTTypeItem ], imported());
}

std::string FileDialog::openCSVFile() {
    return pick(@[ UTTypeCommaSeparatedText, UTTypePlainText ], documents());
}

std::string FileDialog::openPostgresBackupFile() {
    return pick(@[ UTTypeItem ], imported());
}

// saving needs no dialog: the file lands in documents, which the files app shows
std::string FileDialog::pickFolder() {
    return path(documents());
}

std::string FileDialog::savePostgresBackupFile(const std::string& defaultName) {
    return path(freeURL(documents(), [NSString stringWithUTF8String:defaultName.c_str()]));
}

std::string FileDialog::saveFile(const std::string& defaultName, const char*, const char*) {
    return path(freeURL(documents(), [NSString stringWithUTF8String:defaultName.c_str()]));
}
