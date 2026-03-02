#ifdef __APPLE__

#include "platform/alert.hpp"
#import <Cocoa/Cocoa.h>

void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty()) {
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    }

    // NSAlert adds buttons right-to-left: first added = rightmost (primary action)
    // and responds to Return. Order: Default first (rightmost), then Destructive,
    // then Cancel (leftmost). This prevents a destructive button from becoming
    // the default Return-key action when a Default-styled button exists.
    std::vector<size_t> defaultIndices;
    std::vector<size_t> destructiveIndices;
    std::vector<size_t> cancelIndices;
    for (size_t i = 0; i < buttons.size(); ++i) {
        switch (buttons[i].style) {
        case AlertButton::Style::Cancel:
            cancelIndices.push_back(i);
            break;
        case AlertButton::Style::Destructive:
            destructiveIndices.push_back(i);
            break;
        default:
            defaultIndices.push_back(i);
            break;
        }
    }

    std::vector<size_t> orderedIndices;
    orderedIndices.insert(orderedIndices.end(), defaultIndices.begin(), defaultIndices.end());
    orderedIndices.insert(orderedIndices.end(), destructiveIndices.begin(),
                          destructiveIndices.end());
    orderedIndices.insert(orderedIndices.end(), cancelIndices.begin(), cancelIndices.end());

    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title.c_str()]];
        if (!message.empty()) {
            [alert setInformativeText:[NSString stringWithUTF8String:message.c_str()]];
        }

        for (size_t idx : orderedIndices) {
            const auto& btn = buttons[idx];
            NSButton* nsBtn =
                [alert addButtonWithTitle:[NSString stringWithUTF8String:btn.text.c_str()]];

            if (btn.style == AlertButton::Style::Destructive) {
                if (@available(macOS 11.0, *)) {
                    [nsBtn setHasDestructiveAction:YES];
                }
            } else if (btn.style == AlertButton::Style::Cancel) {
                [nsBtn setKeyEquivalent:@"\033"]; // Escape key
            }
        }

        NSModalResponse response = [alert runModal];
        int clickedIndex = static_cast<int>(response - NSAlertFirstButtonReturn);

        if (clickedIndex >= 0 && clickedIndex < static_cast<int>(orderedIndices.size())) {
            size_t originalIndex = orderedIndices[clickedIndex];
            if (buttons[originalIndex].onPress) {
                buttons[originalIndex].onPress();
            }
        }
    }
}

#endif
