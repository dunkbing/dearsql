#import <UIKit/UIKit.h>

#include "platform/alert.hpp"
#include "platform/updater.hpp"
#include "ui/input_dialog.hpp"
#include "utils/cli_command.hpp"

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
