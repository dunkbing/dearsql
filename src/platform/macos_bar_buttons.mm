// native NSButtons for the imgui title bar (ui/title_bar.cpp), placed by mydear::NativeView
#include "imgui.h"
#include "mydear/frames.hpp"
#include "ui/title_bar.hpp"
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface DearSQLBarTarget : NSObject {
@public
    bool* clicked;
}
- (void)fire:(id)sender;
@end

@implementation DearSQLBarTarget
- (void)fire:(id)sender {
    *clicked = true;
    mydear::requestFrames(); // the click came outside imgui: wake the idle loop
}
@end

namespace TitleBarView {
    void* makeNativeButton(const char* symbol, const char* tooltip, bool* clicked) {
        DearSQLBarTarget* target = [DearSQLBarTarget new];
        target->clicked = clicked;
        NSImage* image = [NSImage imageWithSystemSymbolName:@(symbol)
                                   accessibilityDescription:@(tooltip)];
        NSButton* button = [NSButton buttonWithImage:image target:target action:@selector(fire:)];
        button.bezelStyle = NSBezelStyleAccessoryBarAction;
        button.showsBorderOnlyWhileMouseInside = YES;
        button.title = @"";
        button.imagePosition = NSImageOnly; // a title, when set, puts the image after it
        button.toolTip = @(tooltip);
        // the button holds its target weakly
        objc_setAssociatedObject(button, "target", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return (__bridge_retained void*)button;
    }

    float updateNativeButton(void* view, const char* title, ImU32 tint) {
        NSButton* button = (__bridge NSButton*)view;
        const ImVec4 c = ImGui::ColorConvertU32ToFloat4(tint);
        NSColor* color = [NSColor colorWithSRGBRed:c.x green:c.y blue:c.z alpha:c.w];
        if (![button.contentTintColor isEqual:color])
            button.contentTintColor = color;
        if (title) {
            NSDictionary* attrs = @{NSForegroundColorAttributeName : color};
            NSAttributedString* text =
                [[NSAttributedString alloc] initWithString:[@(title) stringByAppendingString:@" "]
                                                attributes:attrs];
            if (![button.attributedTitle isEqual:text])
                button.attributedTitle = text;
            button.imagePosition = NSImageTrailing;
        }
        return button.fittingSize.width;
    }
} // namespace TitleBarView
