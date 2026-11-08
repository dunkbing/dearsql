#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#import "imgui_impl_glfw.h"
#import "imgui_impl_metal.h"
#include "platform/graphics_backend.hpp"
#import <GLFW/glfw3.h>
#import <GLFW/glfw3native.h>

#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string>

namespace {
    // docs screenshots: a frame rendered offscreen at a set scale, saved as png
    struct FrameCapture {
        std::string path;
        float scale = 2;
        int frames = 0; // two: the first bakes the fonts at the new density
        id<MTLTexture> target = nil;
    } capture;

    void savePng(id<MTLTexture> tex, const std::string& path) {
        const size_t w = tex.width, h = tex.height;
        NSMutableData* data = [NSMutableData dataWithLength:w * h * 4];
        [tex getBytes:data.mutableBytes
            bytesPerRow:w * 4
             fromRegion:MTLRegionMake2D(0, 0, w, h)
            mipmapLevel:0];
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGImageRef image = CGImageCreate(w, h, 8, 32, w * 4, cs,
                                         kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                         provider, nullptr, false, kCGRenderingIntentDefault);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
        if (dest) {
            CGImageDestinationAddImage(dest, image, nullptr);
            CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(cs);
    }
} // namespace

void requestFrameCapture(const char* path, float scale) {
    capture.path = path;
    capture.scale = scale;
    capture.frames = 2;
}

bool frameCapturePending() {
    return capture.frames > 0;
}

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

MacOSMetalBackend::MacOSMetalBackend(GLFWwindow* window) : window_(window) {
    metalDevice_ = MTLCreateSystemDefaultDevice();
    if (!metalDevice_) {
        NSLog(@"Failed to create Metal device");
        return;
    }

    metalCommandQueue_ = [(id<MTLDevice>)metalDevice_ newCommandQueue];
    if (!metalCommandQueue_) {
        NSLog(@"Failed to create Metal command queue");
        return;
    }

    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    NSView* contentView = nsWindow.contentView;

    // make the contentView layer-backed so subviews composite correctly
    [contentView setWantsLayer:YES];

    // vibrancy blur behind everything
    PassthroughEffectView* effectView =
        [[PassthroughEffectView alloc] initWithFrame:contentView.bounds];
    effectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    effectView.state = NSVisualEffectStateActive;
    effectView.material = NSVisualEffectMaterialUnderWindowBackground;
    effectView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [contentView addSubview:effectView];

    // Metal rendering view on top of blur, pass-through for events
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
    layer.displaySyncEnabled = YES;
    layer.opaque = NO;
    metalView.layer = layer;
    [metalView setWantsLayer:YES];
    metalLayer_ = layer;

    [appContainer addSubview:metalView];
    [contentView addSubview:appContainer];

    NSLog(@"Metal device and vibrancy layer initialized successfully");
}

bool MacOSMetalBackend::initializeImGui() {
    ImGui_ImplMetal_Init((id<MTLDevice>)metalDevice_);
    NSLog(@"ImGui Metal backend initialized");
    return true;
}

void MacOSMetalBackend::beginFrame(ImVec4 clearColor) {
    ((CAMetalLayer*)metalLayer_).displaySyncEnabled = vsync;
    id<MTLTexture> texture = nil;
    if (capture.frames > 0) {
        int w = 0, h = 0;
        glfwGetWindowSize(window_, &w, &h);
        const auto tw = static_cast<NSUInteger>(w * capture.scale);
        const auto th = static_cast<NSUInteger>(h * capture.scale);
        if (!capture.target || capture.target.width != tw || capture.target.height != th) {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:tw
                                                                  height:th
                                                               mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModeShared;
            capture.target = [(id<MTLDevice>)metalDevice_ newTextureWithDescriptor:d];
        }
        texture = capture.target;
        clearColor.w = 1; // no vibrancy behind a png
        currentDrawable_ = nil;
    } else {
        id<CAMetalDrawable> drawable = [(CAMetalLayer*)metalLayer_ nextDrawable];
        currentDrawable_ = drawable;
        texture = drawable.texture;
    }

    MTLRenderPassDescriptor* renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPassDescriptor.colorAttachments[0].texture = texture;
    renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPassDescriptor.colorAttachments[0].clearColor =
        MTLClearColorMake(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
    renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>)metalCommandQueue_ commandBuffer];
    currentCommandBuffer_ = commandBuffer;

    id<MTLRenderCommandEncoder> renderEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    currentRenderEncoder_ = renderEncoder;

    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
    ImGui_ImplGlfw_NewFrame();
    if (capture.frames > 0) {
        ImGui::GetIO().DisplayFramebufferScale = ImVec2(capture.scale, capture.scale);
    }
}

void MacOSMetalBackend::renderDrawData(ImDrawData* drawData) {
    int display_w, display_h;
    glfwGetFramebufferSize(window_, &display_w, &display_h);
    ((CAMetalLayer*)metalLayer_).drawableSize = CGSizeMake(display_w, display_h);

    ImGui_ImplMetal_RenderDrawData(drawData, (id<MTLCommandBuffer>)currentCommandBuffer_,
                                   (id<MTLRenderCommandEncoder>)currentRenderEncoder_);

    [(id<MTLRenderCommandEncoder>)currentRenderEncoder_ endEncoding];

    [(id<MTLCommandBuffer>)currentCommandBuffer_ commit];

    if (currentDrawable_ == nil && capture.frames > 0 && --capture.frames == 0) {
        [(id<MTLCommandBuffer>)currentCommandBuffer_ waitUntilCompleted];
        savePng(capture.target, capture.path);
        capture.target = nil;
    }
}

void MacOSMetalBackend::present() {
    if (currentDrawable_) {
        [(id<CAMetalDrawable>)currentDrawable_ present];
    }
}

void MacOSMetalBackend::shutdown() {
    ImGui_ImplMetal_Shutdown();
}

void MacOSMetalBackend::getFramebufferSize(int& width, int& height) {
    glfwGetFramebufferSize(window_, &width, &height);
}

ImTextureID MacOSMetalBackend::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
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
