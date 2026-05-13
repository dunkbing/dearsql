#import <Metal/Metal.h>
#import <simd/simd.h>

#include "ui/ripple_shader.hpp"

#include <array>
#include <atomic>
#include <mutex>

namespace {
    id<MTLDevice> gDevice = nil;
    id<MTLRenderCommandEncoder> gEncoder = nil;
    id<MTLRenderPipelineState> gPipeline = nil;
    float gFbWidth = 0.0f;
    float gFbHeight = 0.0f;
    bool gInitFailed = false;

    constexpr int kMaxRipples = 16;
    constexpr float kRippleLifetime = 0.9f; // seconds

    // x, y in imgui points; z = start time, w = unused (also acts as "active" marker via age)
    struct Ripple {
        float x = 0.0f;
        float y = 0.0f;
        float startTime = -1000.0f;
    };

    std::array<Ripple, kMaxRipples> gRipples{};
    int gRippleHead = 0;
    std::mutex gRippleMutex;

    struct GpuUniforms {
        // viewport.x = displayW (points), y = displayH (points), z = currentTime, w = count
        simd_float4 viewport;
        // each ripple: x, y, startTime, unused
        simd_float4 ripples[kMaxRipples];
    };

    NSString* const kShaderSource = @R"msl(
        #include <metal_stdlib>
        using namespace metal;

        constant int kMaxRipples = 16;

        struct Uniforms {
            float4 viewport;
            float4 ripples[kMaxRipples];
        };

        struct VOut {
            float4 pos [[position]];
            float2 uv;
        };

        vertex VOut ripple_vs(uint vid [[vertex_id]]) {
            float2 corners[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
            float2 c = corners[vid];
            VOut o;
            o.pos = float4(c, 0.0, 1.0);
            // uv in 0..1, flipped so (0,0) is top-left to match imgui point space
            o.uv = float2((c.x + 1.0) * 0.5, (1.0 - c.y) * 0.5);
            return o;
        }

        fragment float4 ripple_fs(VOut in [[stage_in]],
                                  constant Uniforms& u [[buffer(0)]]) {
            float2 px = in.uv * u.viewport.xy;
            float currentTime = u.viewport.z;
            int count = int(u.viewport.w);

            float lifetime = 0.9;
            float maxRadius = 260.0;
            float thickness = 14.0;

            float3 rgb = float3(0.0);
            float a = 0.0;

            float3 tint = float3(0.55, 0.78, 1.0);

            for (int i = 0; i < count; ++i) {
                float age = currentTime - u.ripples[i].z;
                if (age < 0.0 || age > lifetime) continue;

                float t = age / lifetime;
                // ease-out for radius, linear fade for opacity
                float eased = 1.0 - pow(1.0 - t, 2.0);
                float radius = eased * maxRadius;
                float fade = 1.0 - t;

                float2 d = px - u.ripples[i].xy;
                float dist = length(d);

                // soft expanding ring
                float ring = exp(-pow((dist - radius) / thickness, 2.0));

                // subtle inner glow that fades faster than the ring
                float inner = exp(-pow(dist / (radius + 30.0), 3.0)) * (1.0 - t) * 0.18;

                float contrib = (ring * 0.55 + inner) * fade;
                rgb += tint * contrib;
                a += contrib;
            }

            a = clamp(a, 0.0, 0.55);
            // premultiplied alpha
            return float4(rgb * a, a);
        }
    )msl";

    bool ensurePipeline() {
        if (gPipeline)
            return true;
        if (gInitFailed)
            return false;
        if (!gDevice)
            return false;

        NSError* err = nil;
        id<MTLLibrary> lib = [gDevice newLibraryWithSource:kShaderSource options:nil error:&err];
        if (!lib) {
            NSLog(@"[RippleShader] shader compile failed: %@", err);
            gInitFailed = true;
            return false;
        }

        id<MTLFunction> vs = [lib newFunctionWithName:@"ripple_vs"];
        id<MTLFunction> fs = [lib newFunctionWithName:@"ripple_fs"];
        if (!vs || !fs) {
            NSLog(@"[RippleShader] missing shader functions");
            gInitFailed = true;
            return false;
        }

        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vs;
        desc.fragmentFunction = fs;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        gPipeline = [gDevice newRenderPipelineStateWithDescriptor:desc error:&err];
        if (!gPipeline) {
            NSLog(@"[RippleShader] pipeline creation failed: %@", err);
            gInitFailed = true;
            return false;
        }
        return true;
    }

} // namespace

namespace RippleShader {

    void setMetalRenderContext(void* device, void* encoder, float fbWidth, float fbHeight) {
        gDevice = (__bridge id<MTLDevice>)device;
        gEncoder = (__bridge id<MTLRenderCommandEncoder>)encoder;
        gFbWidth = fbWidth;
        gFbHeight = fbHeight;
    }

    void notifyClick(float xPoints, float yPoints, float timeSeconds) {
        std::lock_guard<std::mutex> lock(gRippleMutex);
        gRipples[gRippleHead] = Ripple{xPoints, yPoints, timeSeconds};
        gRippleHead = (gRippleHead + 1) % kMaxRipples;
    }

    bool hasActiveRipples(float timeSeconds) {
        std::lock_guard<std::mutex> lock(gRippleMutex);
        for (const auto& r : gRipples) {
            if (timeSeconds - r.startTime <= kRippleLifetime) {
                return true;
            }
        }
        return false;
    }

    void callback(const ImDrawList* /*parentList*/, const ImDrawCmd* /*cmd*/) {
        if (!ensurePipeline() || !gEncoder)
            return;

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (disp.x <= 0 || disp.y <= 0)
            return;

        const float now = static_cast<float>(ImGui::GetTime());

        GpuUniforms u{};
        u.viewport = simd_make_float4(disp.x, disp.y, now, static_cast<float>(kMaxRipples));
        {
            std::lock_guard<std::mutex> lock(gRippleMutex);
            for (int i = 0; i < kMaxRipples; ++i) {
                u.ripples[i] =
                    simd_make_float4(gRipples[i].x, gRipples[i].y, gRipples[i].startTime, 0.0f);
            }
        }

        MTLScissorRect full = {
            0,
            0,
            static_cast<NSUInteger>(gFbWidth),
            static_cast<NSUInteger>(gFbHeight),
        };
        [gEncoder setScissorRect:full];

        [gEncoder setRenderPipelineState:gPipeline];
        [gEncoder setVertexBytes:&u length:sizeof(u) atIndex:0];
        [gEncoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [gEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

} // namespace RippleShader
