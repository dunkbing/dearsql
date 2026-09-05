#pragma once

#include <functional>
#include <string>

// md4c-backed markdown rendering for chat messages: headings, lists, quotes,
// fenced code blocks (with Copy / optional Insert buttons). Inline emphasis
// markers are parsed away; text renders in a single style per block.
namespace MarkdownText {
    using InsertSqlCallback = std::function<void(const std::string&)>;

    // idPrefix keeps ImGui ids unique per message
    void render(const std::string& markdown, const std::string& idPrefix,
                const InsertSqlCallback& insertSql = nullptr);
} // namespace MarkdownText
