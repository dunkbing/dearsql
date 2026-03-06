#pragma once

#include "imgui.h"

namespace UIUtils {
    bool Spinner(const char* label, float radius, int thickness, const ImU32& color);

    // draw a spinner directly on the draw list at a given centre, without affecting layout
    void SpinnerOverlay(ImDrawList* drawList, ImVec2 centre, float radius, int thickness,
                        ImU32 color);
} // namespace UIUtils
