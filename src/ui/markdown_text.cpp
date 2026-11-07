#include "ui/markdown_text.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "themes.hpp"
#include "ui/text_editor.hpp"
#include "utils/button.hpp"
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
            view.editor->SetShowLineNumbers(false);
            view.editor->SetLanguage(languageOf(lang));
        }
        if (view.text != code) { // streaming: the block grows
            view.text = code;
            view.editor->SetText(code);
        }
        view.lastFrame = frame;
        auto palette = dearsql::TextEditor::FromTheme(colors);
        palette.background = ImGui::ColorConvertFloat4ToU32(colors.mantle);
        view.editor->SetPalette(palette);

        // rich_md scopes the block's ids by its text, which changes while it streams:
        // key them by position instead, so the windows below stay the same
        ImGui::PushOverrideID(ImHashStr(key.c_str()));
        ImGui::PushFont(gScope.font, gScope.fontSize); // ours: monospace, with the icons
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
        if (ImGui::BeginChild("##md_code", ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            MarkdownText::copyButton("md_cp_" + key, code);
            if (!lang.empty()) {
                ImGui::SameLine(0, Theme::Spacing::S);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(colors.overlay1, "%s", lang.c_str());
            }
            if (gScope.insertSql && *gScope.insertSql && isSqlLang(lang)) {
                ImGui::SameLine();
                if (UIUtils::SmallButton("Insert##md_ins")) {
                    (*gScope.insertSql)(code);
                }
            }
            const int lines = static_cast<int>(std::ranges::count(code, '\n')) + 1;
            const float height = static_cast<float>(lines) * ImGui::GetTextLineHeightWithSpacing() +
                                 6.0f; // the editor's margins
            view.editor->Render("##md_editor", ImVec2(-1, height));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopID();
    }

    void ensureContext() {
        if (RichMd::GetCurrentContext()) {
            return;
        }
        RichMd::HostServices host = RichMd::GetHostServices();
        host.RenderCodeBlock = renderCode;
        RichMd::SetHostServices(host);
        RichMd::MarkdownOptions options;
        options.hardSoftBreaks = true; // chat text: a newline is a line break
        RichMd::CreateContext(options);
    }
} // namespace

void MarkdownText::render(const std::string& markdown, const std::string& idPrefix,
                          const InsertSqlCallback& insertSql) {
    ensureContext();
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

void MarkdownText::copyButton(const std::string& id, const std::string& text) {
    static ImGuiID copied = 0;
    static double copiedAt = -10.0;
    const auto& colors = Application::getInstance().getCurrentColors();
    const ImGuiID gid = ImGui::GetID(id.c_str());
    const bool done = gid == copied && ImGui::GetTime() - copiedAt < 1.5;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_Text, done ? colors.green : colors.overlay2);
    if (ImGui::SmallButton(
            std::format("{}###{}", done ? ICON_FA_CHECK : ICON_FA_COPY, id).c_str())) {
        ImGui::SetClipboardText(text.c_str());
        copied = gid;
        copiedAt = ImGui::GetTime();
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(done ? "Copied" : "Copy");
    }
}
