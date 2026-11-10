#include "ui/markdown_text.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "mydear/button.hpp"
#include "mydear/embedded.hpp"
#include "themes.hpp"
#include "ui/text_editor.hpp"
#include <algorithm>
#include <format>
#include <imgui_rich_md/rich_md.h>
#include <memory>
#include <unordered_map>

namespace {
    // the render() in progress: code blocks are drawn from inside RichMd::Render
    struct RenderScope {
        const std::string* idPrefix = nullptr;
        const MarkdownText::InsertSqlCallback* insertSql = nullptr;
        int codeBlocks = 0;
        ImFont* font = nullptr; // the caller's, before rich_md pushes its own
        float fontSize = 0.0f;
    };
    RenderScope gScope;

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

    // one read-only editor per code block, for highlighting and selection; dropped
    // once its block has not been drawn for a while
    struct CodeView {
        std::unique_ptr<dearsql::TextEditor> editor;
        std::string text;
        int lastFrame = 0;
    };
    std::unordered_map<std::string, CodeView>& codeViews() {
        static std::unordered_map<std::string, CodeView> views;
        return views;
    }

    // a small framed square icon button
    bool iconButton(const char* id, const char* icon, const char* tooltip,
                    const ImVec4& iconColor) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
        ImGui::PushStyleColor(ImGuiCol_Text, iconColor);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        const float side = ImGui::GetFrameHeight();
        const bool clicked =
            ImGui::Button(std::format("{}###{}", icon, id).c_str(), ImVec2(side, side));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return clicked;
    }

    // shows a check for a moment after copying
    void copyButton(const std::string& id, const std::string& text) {
        static ImGuiID copied = 0;
        static double copiedAt = -10.0;
        const auto& colors = Application::getInstance().getCurrentColors();
        const ImGuiID gid = ImGui::GetID(id.c_str());
        const bool done = gid == copied && ImGui::GetTime() - copiedAt < 1.5;
        if (iconButton(id.c_str(), done ? ICON_FA_CHECK : ICON_FA_COPY, done ? "Copied" : "Copy",
                       done ? colors.green : colors.subtext0)) {
            ImGui::SetClipboardText(text.c_str());
            copied = gid;
            copiedAt = ImGui::GetTime();
        }
    }

    void renderCode(const std::string& rawCode, const std::string& lang) {
        const auto& colors = Application::getInstance().getCurrentColors();
        std::string code = rawCode;
        while (!code.empty() && code.back() == '\n') {
            code.pop_back();
        }

        const int frame = ImGui::GetFrameCount();
        auto& views = codeViews();
        if (frame % 600 == 0) {
            std::erase_if(views, [&](const auto& v) { return frame - v.second.lastFrame > 600; });
        }
        const std::string key =
            std::format("{}_{}", gScope.idPrefix ? *gScope.idPrefix : "md", gScope.codeBlocks++);
        CodeView& view = views[key];
        if (!view.editor) {
            view.editor = std::make_unique<dearsql::TextEditor>();
            view.editor->SetReadOnly(true);
            view.editor->SetShowLineNumbers(true);
            view.editor->SetLanguage(languageOf(lang));
        }
        if (view.text != code) { // streaming: the block grows
            view.text = code;
            view.editor->SetText(code);
        }
        view.lastFrame = frame;
        auto palette = dearsql::TextEditor::FromTheme(colors);
        palette.background = ImGui::ColorConvertFloat4ToU32(colors.crust);
        view.editor->SetPalette(palette);

        // rich_md scopes the block's ids by its text, which changes while it streams:
        // key them by position instead, so the windows below stay the same
        ImGui::PushOverrideID(ImHashStr(key.c_str()));
        ImGui::PushFont(gScope.font, gScope.fontSize); // ours: monospace, with the icons
        // crust + a border: mantle alone blends into the chat
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.crust);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Theme::CornerRadius::SMALL);
        if (ImGui::BeginChild("##md_code", ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            const ImVec2 topRight(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                                  ImGui::GetCursorScreenPos().y);
            const int lines = static_cast<int>(std::ranges::count(code, '\n')) + 1;
            const float height = static_cast<float>(lines) * ImGui::GetTextLineHeightWithSpacing() +
                                 6.0f; // the editor's margins
            view.editor->Render("##md_editor", ImVec2(-1, height));

            // buttons float over the code, top right: a later sibling child draws on top
            const bool insert = gScope.insertSql && *gScope.insertSql && isSqlLang(lang);
            const ImGuiStyle& style = ImGui::GetStyle();
            const float buttonW = ImGui::GetFrameHeight() + style.FramePadding.x;
            const float overlayW = buttonW * (insert ? 2.0f : 1.0f) + Theme::Spacing::S;
            ImGui::SetCursorScreenPos(ImVec2(topRight.x - overlayW, topRight.y));
            if (ImGui::BeginChild("##md_code_actions", ImVec2(overlayW, ImGui::GetFrameHeight()),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
                if (insert) {
                    if (iconButton("md_ins", ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                   "Insert into the editor", colors.subtext0)) {
                        (*gScope.insertSql)(code);
                    }
                    ImGui::SameLine(0, Theme::Spacing::S);
                }
                copyButton("md_cp_" + key, code);
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        ImGui::PopID();
    }

    void ensureContext() {
        if (RichMd::GetCurrentContext()) {
            return;
        }
        RichMd::HostServices host = RichMd::GetHostServices();
        host.RenderCodeBlock = renderCode;
        // the app's font: rich_md asks for <base>-{Regular,Bold}[Italic].ttf and a code font
        host.ReadAsset = [fallback =
                              host.ReadAsset](const std::string& path) -> RichMd::AssetBytes {
            if (path.starts_with("fonts/") && path.ends_with(".ttf") &&
                !path.starts_with("fonts/latex/")) {
                const bool bold = path.ends_with("-Bold.ttf") || path.ends_with("-BoldItalic.ttf");
                const bool italic = path.ends_with("Italic.ttf");
                const char* face =
                    bold ? (italic ? "JetBrainsMono-BoldItalic" : "JetBrainsMono-Bold")
                         : (italic ? "JetBrainsMono-MediumItalic" : "JetBrainsMono-Medium");
                if (const EmbeddedFont* font = findEmbeddedFont(face)) {
                    return std::vector<uint8_t>(font->data, font->data + font->size);
                }
            }
            return fallback ? fallback(path) : RichMd::ReadAssetDefault(path);
        };
        RichMd::SetHostServices(host);
        RichMd::MarkdownOptions options;
        options.hardSoftBreaks = true; // chat text: a newline is a line break
        RichMd::CreateContext(options);
    }
} // namespace

void MarkdownText::render(const std::string& markdown, const std::string& idPrefix,
                          const InsertSqlCallback& insertSql) {
    ensureContext();
    RichMd::GetStyle().codeColor =
        Application::getInstance().getCurrentColors().peach; // inline code
    const RenderScope saved = gScope;
    gScope = {.idPrefix = &idPrefix,
              .insertSql = &insertSql,
              .font = ImGui::GetFont(),
              .fontSize =
                  ImGui::GetCurrentContext()->FontSizeBase}; // unscaled, as PushFont takes it
    ImGui::PushID(idPrefix.c_str());
    RichMd::RenderRaw(markdown);
    ImGui::PopID();
    gScope = saved;
}

void MarkdownText::shutdown() {
    codeViews().clear();
    if (RichMd::GetCurrentContext()) {
        RichMd::DestroyContext();
    }
}
