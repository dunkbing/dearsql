#pragma once

#include <string>

// chat markdown through imshell::Markdown (imgui_rich_md) with this app's fonts, highlighting
// and copy icons on code blocks
namespace MarkdownText {
    // idPrefix keeps ImGui ids unique per message
    void render(const std::string& markdown, const std::string& idPrefix);

    // frees the renderer's fonts and textures; call while the graphics backend is alive
    void shutdown();

} // namespace MarkdownText
