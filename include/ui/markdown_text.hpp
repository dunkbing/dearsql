#pragma once

#include <functional>
#include <string>

// chat markdown through imshell::Markdown (imgui_rich_md) with this app's fonts, highlighting
// and an Insert button on sql code blocks
namespace MarkdownText {
    using InsertSqlCallback = std::function<void(const std::string&)>;

    // idPrefix keeps ImGui ids unique per message
    void render(const std::string& markdown, const std::string& idPrefix,
                const InsertSqlCallback& insertSql = nullptr);

    // frees the renderer's fonts and textures; call while the graphics backend is alive
    void shutdown();

} // namespace MarkdownText
