#import <Metal/Metal.h>
#import <simd/simd.h>

#include "ui/custom_shader.hpp"

#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_msl.hpp>

#include <spdlog/spdlog.h>

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

namespace {
    // valid Vulkan-GLSL preamble matching Ghostty/Shadertoy uniform names, so
    // the same .glsl shaders compile here. member offsets are taken from
    // reflection, not hand-computed, so the std140 layout never has to be
    // mirrored by hand.
    const char* kPrefix = R"glsl(#version 450
layout(binding = 1, std140) uniform Globals {
    vec3  iResolution;
    float iTime;
    float iTimeDelta;
    float iFrameRate;
    int   iFrame;
    float iChannelTime[4];
    vec3  iChannelResolution[4];
    vec4  iMouse;
    vec4  iDate;
    float iSampleRate;
    vec4  iCurrentCursor;
    vec4  iPreviousCursor;
    vec4  iCurrentCursorColor;
    vec4  iPreviousCursorColor;
    int   iCurrentCursorStyle;
    int   iPreviousCursorStyle;
    int   iCursorVisible;
    float iTimeCursorChange;
    float iTimeFocus;
    int   iFocus;
    vec3  iPalette[256];
    vec3  iBackgroundColor;
    vec3  iForegroundColor;
    vec3  iCursorColor;
    vec3  iCursorText;
    vec3  iSelectionForegroundColor;
    vec3  iSelectionBackgroundColor;
    float iEffectOpacity; // dearsql: blend shader output over the UI (0..1)
};

#define CURSORSTYLE_BLOCK        0
#define CURSORSTYLE_BLOCK_HOLLOW 1
#define CURSORSTYLE_BAR          2
#define CURSORSTYLE_UNDERLINE    3
#define CURSORSTYLE_LOCK         4

layout(binding = 0) uniform sampler2D iChannel0;
layout(location = 0) out vec4 _fragColor;

#define texture2D texture

void mainImage(out vec4 fragColor, in vec2 fragCoord);
void main() {
    vec4 shaded;
    mainImage(shaded, gl_FragCoord.xy);
    // blend the effect over the untouched UI so it doesn't fully cover it
    vec4 ui = texture(iChannel0, gl_FragCoord.xy / iResolution.xy);
    _fragColor = mix(ui, shaded, clamp(iEffectOpacity, 0.0, 1.0));
}

)glsl";

    // fullscreen-quad vertex shader (MSL). the transpiled fragment reads
    // gl_FragCoord via [[position]], so no varyings are needed.
    NSString* const kVertexSource = @R"msl(
        #include <metal_stdlib>
        using namespace metal;
        struct VOut { float4 pos [[position]]; };
        vertex VOut fullscreen_vs(uint vid [[vertex_id]]) {
            float2 c[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
            VOut o;
            o.pos = float4(c[vid], 0.0, 1.0);
            return o;
        }
    )msl";

    // ---- transpiled shader state (set by loadFromFile) ----
    bool gLoaded = false;
    std::string gMslSource;
    std::string gEntry; // cleansed MSL fragment entry name (e.g. "main0")
    std::string gPath;
    std::string gLastError; // last load/compile error, surfaced to the UI
    std::unordered_map<std::string, uint32_t> gOffsets; // ubo member -> byte offset
    uint32_t gUboSize = 0;

    // how strongly the shader covers the UI (0..1); fixed so the UI stays legible
    constexpr float kEffectOpacity = 0.55f;

    // ---- Metal pipeline state (built lazily on first render) ----
    id<MTLDevice> gDevice = nil;
    id<MTLRenderPipelineState> gPipeline = nil;
    id<MTLSamplerState> gSampler = nil;
    id<MTLBuffer> gUniformBuffer = nil;
    bool gPipelineFailed = false;

    bool transpile(const std::string& userSource, std::string& err) {
        std::string full = kPrefix + userSource;

        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetSourceLanguage(shaderc_source_language_glsl);
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
        // keep optimization off so OpMemberName debug info survives — we look up
        // uniform offsets by name via reflection. the Metal driver optimizes the
        // final MSL anyway.
        options.SetOptimizationLevel(shaderc_optimization_level_zero);

        shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
            full, shaderc_glsl_fragment_shader, "custom_shader.glsl", options);
        if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
            err = result.GetErrorMessage();
            return false;
        }
        std::vector<uint32_t> spirv(result.cbegin(), result.cend());

        try {
            spirv_cross::CompilerMSL msl(std::move(spirv));

            spirv_cross::CompilerMSL::Options mslOpts;
            mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
            mslOpts.set_msl_version(2, 1);
            msl.set_msl_options(mslOpts);

            // pin resource indices so the Metal side is deterministic:
            //   Globals UBO (binding 1) -> buffer(0)
            //   iChannel0  (binding 0) -> texture(0) + sampler(0)
            spirv_cross::MSLResourceBinding ubo{};
            ubo.stage = spv::ExecutionModelFragment;
            ubo.desc_set = 0;
            ubo.binding = 1;
            ubo.msl_buffer = 0;
            msl.add_msl_resource_binding(ubo);

            spirv_cross::MSLResourceBinding tex{};
            tex.stage = spv::ExecutionModelFragment;
            tex.desc_set = 0;
            tex.binding = 0;
            tex.msl_texture = 0;
            tex.msl_sampler = 0;
            msl.add_msl_resource_binding(tex);

            gMslSource = msl.compile();

            // reflect the uniform block layout
            gOffsets.clear();
            gUboSize = 0;
            spirv_cross::ShaderResources res = msl.get_shader_resources();
            for (const auto& u : res.uniform_buffers) {
                const spirv_cross::SPIRType& type = msl.get_type(u.base_type_id);
                gUboSize = static_cast<uint32_t>(msl.get_declared_struct_size(type));
                for (uint32_t i = 0; i < type.member_types.size(); ++i) {
                    gOffsets[msl.get_member_name(type.self, i)] =
                        msl.type_struct_member_offset(type, i);
                }
            }

            // cleansed fragment entry-point name
            gEntry.clear();
            for (const auto& e : msl.get_entry_points_and_stages()) {
                if (e.execution_model == spv::ExecutionModelFragment) {
                    gEntry = msl.get_cleansed_entry_point_name(e.name, e.execution_model);
                    break;
                }
            }
            if (gEntry.empty())
                gEntry = "main0";
        } catch (const std::exception& e) {
            err = std::string("SPIRV-Cross: ") + e.what();
            return false;
        }
        return true;
    }

    bool ensurePipeline(id<MTLDevice> device) {
        if (gPipeline)
            return true;
        if (gPipelineFailed || !gLoaded || !device)
            return false;
        gDevice = device;

        NSError* nsErr = nil;
        id<MTLLibrary> vlib = [gDevice newLibraryWithSource:kVertexSource options:nil error:&nsErr];
        if (!vlib) {
            spdlog::error("[CustomShader] vertex library failed: {}",
                          nsErr.localizedDescription.UTF8String);
            gPipelineFailed = true;
            return false;
        }
        NSString* fsrc = [NSString stringWithUTF8String:gMslSource.c_str()];
        id<MTLLibrary> flib = [gDevice newLibraryWithSource:fsrc options:nil error:&nsErr];
        if (!flib) {
            spdlog::error("[CustomShader] fragment library failed: {}",
                          nsErr.localizedDescription.UTF8String);
            gPipelineFailed = true;
            return false;
        }

        id<MTLFunction> vfn = [vlib newFunctionWithName:@"fullscreen_vs"];
        id<MTLFunction> ffn =
            [flib newFunctionWithName:[NSString stringWithUTF8String:gEntry.c_str()]];
        if (!vfn || !ffn) {
            spdlog::error("[CustomShader] missing shader functions (entry '{}')", gEntry);
            gPipelineFailed = true;
            return false;
        }

        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vfn;
        desc.fragmentFunction = ffn;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = NO;

        gPipeline = [gDevice newRenderPipelineStateWithDescriptor:desc error:&nsErr];
        if (!gPipeline) {
            spdlog::error("[CustomShader] pipeline creation failed: {}",
                          nsErr.localizedDescription.UTF8String);
            gPipelineFailed = true;
            return false;
        }

        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        gSampler = [gDevice newSamplerStateWithDescriptor:sd];

        NSUInteger bufLen = gUboSize > 0 ? gUboSize : 16;
        gUniformBuffer = [gDevice newBufferWithLength:bufLen options:MTLResourceStorageModeShared];
        return gSampler && gUniformBuffer;
    }

    void fillUniforms() {
        auto* base = static_cast<uint8_t*>(gUniformBuffer.contents);
        memset(base, 0, gUniformBuffer.length);

        auto put = [&](const char* name, const void* data, size_t sz) {
            auto it = gOffsets.find(name);
            if (it != gOffsets.end() && it->second + sz <= gUniformBuffer.length)
                memcpy(base + it->second, data, sz);
        };

        ImGuiIO& io = ImGui::GetIO();

        const float resW = io.DisplaySize.x * io.DisplayFramebufferScale.x;
        const float resH = io.DisplaySize.y * io.DisplayFramebufferScale.y;
        const float res[3] = {resW, resH, 1.0f};
        const float chRes[3] = {resW, resH, 1.0f};
        const float t = static_cast<float>(ImGui::GetTime());
        const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;
        const float fr = 1.0f / dt;
        const int frame = ImGui::GetFrameCount();
        const float sampleRate = 44100.0f;

        const bool down = io.MouseDown[0];
        const float mx = io.MousePos.x * io.DisplayFramebufferScale.x;
        const float my = io.MousePos.y * io.DisplayFramebufferScale.y;
        const float mouse[4] = {down ? mx : 0.0f, down ? my : 0.0f, down ? mx : -mx,
                                down ? my : -my};

        std::time_t now = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&now, &tmv);
        const float date[4] = {
            static_cast<float>(tmv.tm_year + 1900), static_cast<float>(tmv.tm_mon),
            static_cast<float>(tmv.tm_mday),
            static_cast<float>(tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec)};
        const int focus = 1;
        const int cursorVisible = 1;

        put("iResolution", res, sizeof(res));
        put("iTime", &t, sizeof(t));
        put("iTimeDelta", &dt, sizeof(dt));
        put("iFrameRate", &fr, sizeof(fr));
        put("iFrame", &frame, sizeof(frame));
        put("iChannelTime", &t, sizeof(t));
        put("iChannelResolution", chRes, sizeof(chRes));
        put("iMouse", mouse, sizeof(mouse));
        put("iDate", date, sizeof(date));
        put("iSampleRate", &sampleRate, sizeof(sampleRate));
        put("iFocus", &focus, sizeof(focus));
        put("iCursorVisible", &cursorVisible, sizeof(cursorVisible));
        const float opacity = kEffectOpacity;
        put("iEffectOpacity", &opacity, sizeof(opacity));
    }

} // namespace

namespace CustomShader {

    bool isLoaded() {
        return gLoaded;
    }

    void unload() {
        gLoaded = false;
        gPath.clear();
        gLastError.clear();
    }

    const std::string& loadedPath() {
        return gPath;
    }

    const std::string& lastError() {
        return gLastError;
    }

    bool loadFromFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            gLastError = "cannot open shader file: " + path;
            spdlog::error("[CustomShader] {}", gLastError);
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        if (src.empty()) {
            gLastError = "shader file is empty: " + path;
            spdlog::error("[CustomShader] {}", gLastError);
            return false;
        }

        std::string err;
        if (!transpile(src, err)) {
            gLastError = err;
            spdlog::error("[CustomShader] failed to compile '{}':\n{}", path, err);
            return false;
        }

        // stage the new shader; the pipeline rebuilds on next render
        gLastError.clear();
        gLoaded = true;
        gPath = path;
        gPipeline = nil;
        gSampler = nil;
        gUniformBuffer = nil;
        gPipelineFailed = false;
        spdlog::info("[CustomShader] loaded shader: {}", path);
        return true;
    }

    void render(void* device, void* commandBuffer, void* sceneTexture, void* drawableTexture) {
        if (!gLoaded)
            return;
        id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
        id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)commandBuffer;
        id<MTLTexture> scene = (__bridge id<MTLTexture>)sceneTexture;
        id<MTLTexture> drawable = (__bridge id<MTLTexture>)drawableTexture;
        if (!dev || !cmd || !scene || !drawable)
            return;

        // fallback: if the pipeline can't be built, copy the scene through so
        // the app stays visible instead of going black.
        if (!ensurePipeline(dev)) {
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit copyFromTexture:scene toTexture:drawable];
            [blit endEncoding];
            return;
        }

        fillUniforms();

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:gPipeline];
        [enc setFragmentBuffer:gUniformBuffer offset:0 atIndex:0];
        [enc setFragmentTexture:scene atIndex:0];
        [enc setFragmentSamplerState:gSampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
    }

} // namespace CustomShader
