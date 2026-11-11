#pragma once
#include "imshell/editor.hpp"
#include <cstdint>
#include <dearsql/completion.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dearsql {

    // imshell's editor with this app's languages: tree-sitter for SQL and JSON, a lexer for
    // Redis. new editors are SQL
    class TextEditor : public imshell::TextEditor {
    public:
        TextEditor();

        enum class Language : uint8_t { SQL, Redis, JSON, PlainText };
        // highlighting, the line comment, enter indent and (SQL) keyword completion
        void SetLanguage(Language language);

        [[nodiscard]] static std::string FormatSQL(const std::string& sql);
        [[nodiscard]] static std::string FormatJSON(const std::string& json);

        // a completion row for a catalog kind: its letter and colour in the popup
        [[nodiscard]] static CompletionItem Row(std::string text, CompletionKind kind);

        // tree-sitter or the redis lexer (text_editor_highlight.cpp); none for plain text. also
        // the chat's code blocks
        static Highlighter highlighterFor(Language language);
    };

    // completion from a fixed list (redis commands, mongo operators and fields): the items
    // matching the word before the cursor, prefix matches first; qualifiers ("db.coll")
    // narrow them after a dot
    struct ListCompletion {
        std::string text;
        std::string insertText; // empty: text
        std::string matchText;  // empty: text
        std::string detailText;
        std::vector<std::string> qualifiers;
        CompletionKind kind = CompletionKind::Keyword;
        ListCompletion() = default;
        ListCompletion(std::string t, CompletionKind k) : text(std::move(t)), kind(k) {}
    };
    struct ListCompletionRequest {
        std::string_view content;
        int cursorIndex = 0;
        std::string_view currentWord;
        std::vector<std::string> qualifierParts;
        bool forced = false;
    };
    // replaces the built-in matching (redis matches arguments by position)
    using ListCompletionFilter = std::function<std::vector<ListCompletion>(
        const ListCompletionRequest&, const std::vector<ListCompletion>&)>;
    [[nodiscard]] TextEditor::CompletionProvider listCompletions(std::vector<ListCompletion> items,
                                                                 ListCompletionFilter filter = {});

} // namespace dearsql
