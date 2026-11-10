#include "ui/fonts.hpp"

#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "mydear/embedded.hpp"
#include "mydear/fonts.hpp"
#include <algorithm>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <string_view>
#include <vector>

namespace {
    const ImWchar kFontAwesomeRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    const ImWchar kForkAwesomeRanges[] = {ICON_MIN_FK, ICON_MAX_16_FK, 0};

    // bold and italic faces are for chat markdown only (markdown_text.cpp), not the ui font
    bool isMarkdownFace(const std::string_view name) {
        return name.find("Bold") != std::string_view::npos ||
               name.find("Italic") != std::string_view::npos;
    }

    mydear::FontFile fontFile(const EmbeddedFont& font) {
        const std::string_view name = font.name;
        mydear::FontFile f{font.data, font.size};
        if (name.find("fa-solid") != name.npos || name.find("fa-regular") != name.npos)
            f.ranges = kFontAwesomeRanges, f.icons = true;
        else if (name.find("forkawesome") != name.npos)
            f.ranges = kForkAwesomeRanges, f.icons = true;
        else if (name.find("Cyrillic") != name.npos)
            f.ranges = ImGui::GetIO().Fonts->GetGlyphRangesCyrillic();
        return f;
    }
} // namespace

void fonts::setup() {
    // the monospace text font first (the base), then icons and other scripts merged into it,
    // then the system cjk font: db, schema and table names are often cjk
    std::vector<mydear::FontFile> files;
    const EmbeddedFont* all = getEmbeddedFonts();
    for (size_t i = 0; i < getEmbeddedFontCount(); ++i) {
        if (isMarkdownFace(all[i].name))
            continue;
        files.push_back(fontFile(all[i]));
    }
    std::ranges::stable_partition(files, [](const auto& f) { return !f.icons && !f.ranges; });
    if (files.empty())
        return;
    if (!mydear::AddFont(files))
        spdlog::error("could not load the ui font");
    const std::string cjk = mydear::SystemCjkFontPath();
    if (cjk.empty())
        spdlog::info("no system cjk font found, cjk text will not render");
    else
        spdlog::info("merged system cjk font: {}", cjk);
}
