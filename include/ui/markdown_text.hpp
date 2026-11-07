#pragma once

#include <functional>
#include <string>

// chat markdown through imgui_rich_md: inline styles, links, tables, mermaid, and text
// that can be selected and copied. code blocks are ours: highlighted and selectable,
// with a copy icon and an optional Insert.
namespace MarkdownText {
    using InsertSqlCallback = std::function<void(const std::string&)>;

    // idPrefix keeps ImGui ids unique per message
    void render(const std::string& markdown, const std::string& idPrefix,
                const InsertSqlCallback& insertSql = nullptr);

    // frees the renderer's fonts and textures; call while the graphics backend is alive
    void shutdown();

    // borderless copy icon; shows a check for a moment after copying
    void copyButton(const std::string& id, const std::string& text);
} // namespace MarkdownText
