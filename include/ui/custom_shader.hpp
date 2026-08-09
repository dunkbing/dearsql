#pragma once

#include <string>

// runtime-loadable Shadertoy-style GLSL shaders, macOS / Metal only.
//
// works like Ghostty's custom-shader: a .glsl file with the standard
//   void mainImage(out vec4 fragColor, in vec2 fragCoord)
// entry point is transpiled GLSL -> SPIR-V (shaderc) -> MSL (SPIRV-Cross) at
// load time. the rendered app is bound as iChannel0; the usual Shadertoy
// uniforms (iResolution, iTime, iTimeDelta, iFrame, iMouse, iDate, ...) are
// provided. when a shader is loaded it runs as a fullscreen post-process pass,
// replacing the built-in black hole.
namespace CustomShader {

    // read + transpile the shader at `path`. returns false and logs on a read
    // or compile error (leaving any previously-loaded shader in place). safe to
    // call before the Metal device exists — the pipeline is built lazily on the
    // first render.
    bool loadFromFile(const std::string& path);

    // true once a shader has been transpiled successfully.
    bool isLoaded();

    // turn the shader off at runtime (next frame renders the app normally).
    void unload();

    // path of the currently loaded shader, or "" if none.
    const std::string& loadedPath();

    // last load/compile error message ("" if the last load succeeded).
    const std::string& lastError();

#ifdef __APPLE__
    // fullscreen post-process pass: sample sceneTexture as iChannel0 and write
    // the shader output to the drawable. all void* args are id<MTLDevice> /
    // id<MTLCommandBuffer> / id<MTLTexture>. no-op if nothing is loaded.
    void render(void* device, void* commandBuffer, void* sceneTexture, void* drawableTexture);
#endif

} // namespace CustomShader
