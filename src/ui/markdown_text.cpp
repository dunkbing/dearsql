#include "ui/markdown_text.hpp"

#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"
#include <format>
#include <md4c.h>
#include <vector>

namespace {

    struct Block {
        enum class Type { Paragraph, Heading, Code, ListItem, Quote, Rule };
        Type type = Type::Paragraph;
        std::string text;
        std::string lang; // code fence info
        int level = 0;    // heading level
        int indent = 0;   // list nesting depth
        bool ordered = false;
        int number = 0; // ordered list item number
    };

    struct ParseState {
        std::vector<Block> blocks;
        int listDepth = 0;
        int quoteDepth = 0;
        std::vector<int> orderedCounters; // -1 = unordered level
        bool inCode = false;

        Block& current() {
            if (blocks.empty()) {
                blocks.emplace_back();
            }
            return blocks.back();
        }

        void open(Block::Type type) {
            Block b;
            b.type = quoteDepth > 0 && type == Block::Type::Paragraph ? Block::Type::Quote : type;
            b.indent = listDepth;
            blocks.push_back(std::move(b));
        }
    };

    int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
        auto& st = *static_cast<ParseState*>(userdata);
        switch (type) {
        case MD_BLOCK_H:
            st.open(Block::Type::Heading);
            st.current().level = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
            break;
        case MD_BLOCK_CODE: {
            st.open(Block::Type::Code);
            st.inCode = true;
            const auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            if (d->lang.text != nullptr) {
                st.current().lang.assign(d->lang.text, d->lang.size);
            }
            break;
        }
        case MD_BLOCK_UL:
            st.listDepth++;
            st.orderedCounters.push_back(-1);
            break;
        case MD_BLOCK_OL: {
            st.listDepth++;
            const auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            st.orderedCounters.push_back(static_cast<int>(d->start));
            break;
        }
        case MD_BLOCK_LI:
            st.open(Block::Type::ListItem);
            st.current().indent = st.listDepth;
            if (!st.orderedCounters.empty() && st.orderedCounters.back() >= 0) {
                st.current().ordered = true;
                st.current().number = st.orderedCounters.back()++;
            }
            break;
        case MD_BLOCK_QUOTE:
            st.quoteDepth++;
            break;
        case MD_BLOCK_HR:
            st.open(Block::Type::Rule);
            break;
        case MD_BLOCK_P:
            // first paragraph of a list item flows into the item block
            if (st.blocks.empty() || st.blocks.back().type != Block::Type::ListItem ||
                !st.blocks.back().text.empty()) {
                st.open(Block::Type::Paragraph);
            }
            break;
        default:
            break;
        }
        return 0;
    }

    int leaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
        auto& st = *static_cast<ParseState*>(userdata);
        switch (type) {
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            st.listDepth--;
            if (!st.orderedCounters.empty()) {
                st.orderedCounters.pop_back();
            }
            break;
        case MD_BLOCK_QUOTE:
            st.quoteDepth--;
            break;
        case MD_BLOCK_CODE:
            st.inCode = false;
            break;
        default:
            break;
        }
        return 0;
    }

    int enterSpan(MD_SPANTYPE /*type*/, void* /*detail*/, void* /*userdata*/) {
        return 0; // emphasis/code-span markers stripped; text arrives via textCb
    }

    int leaveSpan(MD_SPANTYPE type, void* detail, void* userdata) {
        if (type == MD_SPAN_A) {
            const auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
            if (d->href.text != nullptr && d->href.size > 0) {
                auto& st = *static_cast<ParseState*>(userdata);
                st.current().text += " (" + std::string(d->href.text, d->href.size) + ")";
            }
        }
        return 0;
    }

    int textCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
        auto& st = *static_cast<ParseState*>(userdata);
        switch (type) {
        case MD_TEXT_SOFTBR:
            st.current().text += st.inCode ? "\n" : " ";
            break;
        case MD_TEXT_BR:
            st.current().text += "\n";
            break;
        case MD_TEXT_NULLCHAR:
            break;
        default:
            st.current().text.append(text, size);
            break;
        }
        return 0;
    }

    std::vector<Block> parse(const std::string& markdown) {
        ParseState state;
        MD_PARSER parser{};
        parser.abi_version = 0;
        parser.flags = MD_FLAG_NOHTML | MD_FLAG_STRIKETHROUGH;
        parser.enter_block = enterBlock;
        parser.leave_block = leaveBlock;
        parser.enter_span = enterSpan;
        parser.leave_span = leaveSpan;
        parser.text = textCb;
        md_parse(markdown.c_str(), static_cast<MD_SIZE>(markdown.size()), &parser, &state);
        return state.blocks;
    }

    bool isSqlLang(const std::string& lang) {
        return lang.empty() || lang == "sql" || lang == "SQL" || lang == "sqlite" ||
               lang == "postgresql" || lang == "mysql" || lang == "json";
    }

    void renderCode(const Block& block, const std::string& idPrefix, size_t idx,
                    const MarkdownText::InsertSqlCallback& insertSql) {
        const auto& colors = Application::getInstance().getCurrentColors();
        std::string code = block.text;
        while (!code.empty() && code.back() == '\n') {
            code.pop_back();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
        const std::string childId = std::format("##md_code_{}_{}", idPrefix, idx);
        if (ImGui::BeginChild(childId.c_str(), ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            if (UIUtils::SmallButton(std::format("Copy##md_cp_{}_{}", idPrefix, idx).c_str())) {
                ImGui::SetClipboardText(code.c_str());
            }
            if (insertSql && isSqlLang(block.lang)) {
                ImGui::SameLine();
                if (UIUtils::SmallButton(
                        std::format("Insert##md_ins_{}_{}", idPrefix, idx).c_str())) {
                    insertSql(code);
                }
            }
            ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
            ImGui::TextWrapped("%s", code.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

} // namespace

void MarkdownText::render(const std::string& markdown, const std::string& idPrefix,
                          const InsertSqlCallback& insertSql) {
    const auto& colors = Application::getInstance().getCurrentColors();
    const auto blocks = parse(markdown);
    const float wrapWidth = ImGui::GetContentRegionAvail().x - Theme::Spacing::M;

    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& block = blocks[i];
        if (block.text.empty() && block.type != Block::Type::Rule) {
            continue;
        }
        switch (block.type) {
        case Block::Type::Heading:
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            ImGui::TextColored(colors.blue, "%s", block.text.c_str());
            ImGui::PopTextWrapPos();
            if (block.level <= 2) {
                ImGui::Separator();
            }
            break;
        case Block::Type::Code:
            renderCode(block, idPrefix, i, insertSql);
            break;
        case Block::Type::ListItem: {
            const float indent = Theme::Spacing::M * static_cast<float>(block.indent);
            ImGui::Indent(indent);
            const std::string marker =
                block.ordered ? std::format("{}.", block.number) : std::string("-");
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth - indent);
            ImGui::TextWrapped("%s %s", marker.c_str(), block.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(indent);
            break;
        }
        case Block::Type::Quote:
            ImGui::Indent(Theme::Spacing::M);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth - Theme::Spacing::M);
            ImGui::TextColored(colors.subtext0, "%s", block.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(Theme::Spacing::M);
            break;
        case Block::Type::Rule:
            ImGui::Separator();
            break;
        case Block::Type::Paragraph:
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            ImGui::TextWrapped("%s", block.text.c_str());
            ImGui::PopTextWrapPos();
            break;
        }
    }
}
