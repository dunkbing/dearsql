#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "platform/ios_platform.hpp"
#include "ui/alert.hpp"
#include "utils/cli_command.hpp"
#include "utils/file_dialog.hpp"

#include <filesystem>
#include <memory>

namespace {
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
} // namespace

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
@property(nonatomic, copy) void (^done)(NSURL*);
@end

@implementation DearSQLPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)picker
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    self.done(urls.firstObject);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)picker {
    self.done(nil);
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

    // a file of ours as it is, anything else copied in: sqlite needs to create its journal
    // next to the database, and access to a picked file does not cover its folder
    std::string localCopy(NSURL* url, NSURL* into) {
        NSString* container = NSHomeDirectory().stringByStandardizingPath;
        if ([url.path.stringByStandardizingPath hasPrefix:container])
            return path(url);
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

    DearSQLPickerDelegate* gPicking = nil; // the picker holds its delegate weakly

    void pick(NSArray<UTType*>* types, NSURL* into, FileDialog::Done done) {
        UIViewController* top = topController();
        if (!top)
            return;
        UIDocumentPickerViewController* picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
        gPicking = [DearSQLPickerDelegate new];
        gPicking.done = ^(NSURL* url) {
          gPicking = nil;
          [[top.view.window viewWithTag:kDearSQLViewTag] becomeFirstResponder];
          if (!url)
              return;
          if (const std::string file = localCopy(url, into); !file.empty())
              done(file);
        };
        picker.delegate = gPicking;
        [top presentViewController:picker animated:YES completion:nil];
    }
} // namespace

bool FileDialog::isInitialized = true;

bool FileDialog::initialize() {
    return true;
}

void FileDialog::cleanup() {}

void FileDialog::openDatabaseFile(DatabaseType, Done done) {
    pick(@[ UTTypeData ], documents(), std::move(done));
}

void FileDialog::openFile(Done done) {
    pick(@[ UTTypeItem ], imported(), std::move(done));
}

void FileDialog::openCSVFile(Done done) {
    pick(@[ UTTypeCommaSeparatedText, UTTypePlainText ], documents(), std::move(done));
}

void FileDialog::openPostgresBackupFile(Done done) {
    pick(@[ UTTypeItem ], imported(), std::move(done));
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
