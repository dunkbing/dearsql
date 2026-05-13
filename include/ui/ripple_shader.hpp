#pragma once

#include "imgui.h"

// click ripple effect, macOS / Metal only. gated by DEARSQL_EXPERIMENTAL in CMake.
namespace RippleShader {

    // record a click at the given imgui point-space coordinate, current time
    void notifyClick(float xPoints, float yPoints, float timeSeconds);

    // matches ImDrawCallback. cmd->UserCallbackData is ignored; state lives in the module.
    void callback(const ImDrawList* parentList, const ImDrawCmd* cmd);

    // true if any ripple is still animating (caller uses this to keep frames pumping)
    bool hasActiveRipples(float timeSeconds);

#ifdef __APPLE__
    // called once per frame by the Metal backend before ImGui renders its draw data.
    // void* keeps this header Metal-free for portable callers.
    void setMetalRenderContext(void* device, void* encoder, float fbWidth, float fbHeight);
#endif

} // namespace RippleShader
