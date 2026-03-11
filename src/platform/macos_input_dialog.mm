#ifdef __APPLE__

#include "ui/input_dialog.hpp"
#import <Cocoa/Cocoa.h>

void InputDialog::show(const std::string& title, const std::string& label,
                       const std::string& initialValue, const std::string& confirmButtonText,
                       ConfirmCallback onConfirm, CancelCallback onCancel,
                       ValidatorCallback validator) {
    @autoreleasepool {
        NSString* currentValue = [NSString stringWithUTF8String:initialValue.c_str()];
        std::string errorMsg;

        while (true) {
            NSAlert* alert = [[NSAlert alloc] init];
            [alert setMessageText:[NSString stringWithUTF8String:title.c_str()]];

            if (!errorMsg.empty()) {
                [alert setInformativeText:[NSString stringWithUTF8String:errorMsg.c_str()]];
                [alert setAlertStyle:NSAlertStyleWarning];
            } else if (!label.empty()) {
                [alert setInformativeText:[NSString stringWithUTF8String:label.c_str()]];
            }

            NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
            [input setStringValue:currentValue];
            [alert setAccessoryView:input];

            [alert addButtonWithTitle:[NSString stringWithUTF8String:confirmButtonText.c_str()]];
            NSButton* cancelBtn = [alert addButtonWithTitle:@"Cancel"];
            [cancelBtn setKeyEquivalent:@"\033"];

            [alert layout];
            [[alert window] setInitialFirstResponder:input];

            NSModalResponse response = [alert runModal];

            if (response != NSAlertFirstButtonReturn) {
                if (onCancel)
                    onCancel();
                [[NSApp mainWindow] makeKeyWindow];
                return;
            }

            std::string value = [[input stringValue] UTF8String];
            currentValue = [input stringValue];

            if (validator) {
                errorMsg = validator(value);
                if (!errorMsg.empty())
                    continue;
            }

            if (onConfirm) {
                errorMsg = onConfirm(value);
                if (!errorMsg.empty())
                    continue;
            }

            [[NSApp mainWindow] makeKeyWindow];
            return;
        }
    }
}

#endif
