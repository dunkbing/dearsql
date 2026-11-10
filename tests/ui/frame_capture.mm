#include "frame_capture.hpp"

#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "mydear/themes.hpp"
#include "mydear/title_bar.hpp"

#include <cstdio>
#include <format>
#include <vector>

namespace {
    id<MTLCommandQueue> queue;

    struct {
        std::string path;
        float scale = 2;
        bool pending = false;
    } shot;

    struct {
        FILE* pipe = nullptr;
        float scale = 1;
        NSUInteger w = 0, h = 0;
        std::vector<uint8_t> buf;
    } rec;

    // the last frame's draw data into a cpu-readable texture of w x h at `scale`. the default
    // device is the one mydear's backend uses, so the font atlas and other textures resolve
    id<MTLTexture> drawAgain(float scale, NSUInteger w, NSUInteger h) {
        ImDrawData* data = ImGui::GetDrawData();
        if (!data || !data->Valid || w == 0 || h == 0)
            return nil;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!queue)
            queue = [device newCommandQueue];
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:d];

        const ImVec4 base = mydear::Theme::Current()->colors.base;
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(base.x, base.y, base.z, 1);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        // shortcut: glyphs stay baked at the window's density, so a scale above the display's
        // softens text; docs shots run on a retina display at 2x
        const ImVec2 scaleBefore = data->FramebufferScale;
        data->FramebufferScale = ImVec2(scale, scale);
        ImGui_ImplMetal_NewFrame(pass); // the pipeline for this target; the next frame resets it
        ImGui_ImplMetal_RenderDrawData(data, commands, encoder);
        data->FramebufferScale = scaleBefore;
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        return target;
    }

    // top: rows to leave out (the title bar strip: its os buttons are native, not in the frame)
    void savePng(id<MTLTexture> tex, const std::string& path, size_t top) {
        const size_t w = tex.width, h = tex.height - top;
        NSMutableData* data = [NSMutableData dataWithLength:w * h * 4];
        [tex getBytes:data.mutableBytes
            bytesPerRow:w * 4
             fromRegion:MTLRegionMake2D(0, top, w, h)
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

namespace frame_capture {
    void requestPng(const std::string& path, float scale) {
        shot.path = path;
        shot.scale = scale;
        shot.pending = true;
    }

    bool pending() {
        return shot.pending;
    }

    bool startRecording(const char* path, float scale, int width, int height) {
        rec.scale = scale;
        rec.w = static_cast<NSUInteger>(width * scale);
        rec.h = static_cast<NSUInteger>(height * scale);
        rec.buf.resize(rec.w * rec.h * 4);
        // wall-clock stamps: the video runs as fast as the app did, no faster
        const std::string cmd =
            std::format("ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s {}x{} "
                        "-use_wallclock_as_timestamps 1 "
                        "-i - -fps_mode cfr -r 60 -c:v h264_videotoolbox -b:v 12M -pix_fmt yuv420p "
                        "-movflags +faststart '{}'",
                        rec.w, rec.h, path);
        rec.pipe = popen(cmd.c_str(), "w");
        return rec.pipe != nullptr;
    }

    void stopRecording() {
        if (rec.pipe) {
            pclose(rec.pipe);
            rec.pipe = nullptr;
        }
    }

    void afterFrame() {
        if (shot.pending) {
            const ImVec2 size = ImGui::GetIO().DisplaySize;
            if (id<MTLTexture> tex =
                    drawAgain(shot.scale, static_cast<NSUInteger>(size.x * shot.scale),
                              static_cast<NSUInteger>(size.y * shot.scale)))
                savePng(tex, shot.path, static_cast<size_t>(mydear::TitleBarHeight() * shot.scale));
            shot.pending = false;
        }
        if (rec.pipe) {
            if (id<MTLTexture> tex = drawAgain(rec.scale, rec.w, rec.h)) {
                [tex getBytes:rec.buf.data()
                    bytesPerRow:rec.w * 4
                     fromRegion:MTLRegionMake2D(0, 0, rec.w, rec.h)
                    mipmapLevel:0];
                fwrite(rec.buf.data(), 1, rec.buf.size(), rec.pipe);
            }
        }
    }
} // namespace frame_capture
