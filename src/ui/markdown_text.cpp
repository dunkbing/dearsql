#include "ui/markdown_text.hpp"

#include "IconsFontAwesome6.h"
#include "imshell/embedded.hpp"
#include "imshell/markdown.hpp"
#include "ui/text_editor.hpp"

namespace {
    bool isSqlLang(const std::string& lang) {
        return lang.empty() || lang == "sql" || lang == "SQL" || lang == "sqlite" ||
               lang == "postgresql" || lang == "mysql" || lang == "json";
    }

    dearsql::TextEditor::Language languageOf(const std::string& lang) {
        if (lang == "json")
            return dearsql::TextEditor::Language::JSON;
        if (lang == "redis")
            return dearsql::TextEditor::Language::Redis;
        return isSqlLang(lang) ? dearsql::TextEditor::Language::SQL
                               : dearsql::TextEditor::Language::PlainText;
    }

    imshell::Markdown::Font font(const char* name) {
        const EmbeddedFont* f = findEmbeddedFont(name);
        return f ? imshell::Markdown::Font{f->data, f->size} : imshell::Markdown::Font{};
    }

    void configure() {
        static bool done = false;
        if (done)
            return;
        done = true;
        imshell::Markdown::Options options;
        options.regular = options.code = font("JetBrainsMono-Medium");
        options.bold = font("JetBrainsMono-Bold");
        options.italic = font("JetBrainsMono-MediumItalic");
        options.boldItalic = font("JetBrainsMono-BoldItalic");
        options.highlighter = [](const std::string& lang) {
            return dearsql::TextEditor::highlighterFor(languageOf(lang));
        };
        options.copyIcon = ICON_FA_COPY;
        options.copiedIcon = ICON_FA_CHECK;
        imshell::Markdown::Configure(std::move(options));
    }
} // namespace

void MarkdownText::render(const std::string& markdown, const std::string& idPrefix) {
    configure();
    imshell::Markdown::Render(markdown, idPrefix);
}

void MarkdownText::shutdown() {
    imshell::Markdown::Shutdown();
}
