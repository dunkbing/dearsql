#include "ui/fonts.hpp"

#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "mydear/embedded.hpp"
#include <filesystem>
#include <imgui.h>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
    ImFont* gTabFont = nullptr;

    const ImWchar kFontAwesomeRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    const ImWchar kForkAwesomeRanges[] = {ICON_MIN_FK, ICON_MAX_16_FK, 0};

    // a font file mapped read-only for the whole run. its pages stay clean: the OS
    // loads only the glyph data drawn and can drop it again, where a read copy of a
    // 23MB .ttc stays resident. empty if it can't be mapped
    std::span<char> mapFontFile(const std::string& path) {
#ifdef _WIN32
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};
        LARGE_INTEGER size{};
        HANDLE mapping = GetFileSizeEx(file, &size)
                             ? CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr)
                             : nullptr;
        CloseHandle(file);
        if (!mapping)
            return {};
        void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping); // the view keeps the mapping alive
        return data ? std::span(static_cast<char*>(data), static_cast<size_t>(size.QuadPart))
                    : std::span<char>{};
#else
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            return {};
        struct stat st{};
        void* data = fstat(fd, &st) == 0 && st.st_size > 0
                         ? mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)
                         : MAP_FAILED;
        close(fd); // the mapping keeps the file open
        return data == MAP_FAILED
                   ? std::span<char>{}
                   : std::span(static_cast<char*>(data), static_cast<size_t>(st.st_size));
#endif
    }

    // probe common system locations for a CJK-capable font; first hit wins
    std::string findSystemCjkFontPath() {
        static const char* candidates[] = {
#if defined(__APPLE__)
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/PingFang.ttc",
            "/Library/Fonts/Arial Unicode.ttf",
#elif defined(_WIN32)
            "C:\\Windows\\Fonts\\msyh.ttc",    // microsoft yahei (SC)
            "C:\\Windows\\Fonts\\YuGothM.ttc", // yu gothic (JP)
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\msgothic.ttc",
#else
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
            "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
#endif
        };
        for (const char* path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
        return {};
    }

    struct EmbeddedFontSpec {
        const ImWchar* ranges = nullptr;
        bool isIconFont = false;
        bool isPrimaryTextFont = false;
    };

    // bold and italic faces are for chat markdown only (markdown_text.cpp), not the ui font
    bool isMarkdownFace(const std::string_view name) {
        return name.find("Bold") != std::string_view::npos ||
               name.find("Italic") != std::string_view::npos;
    }

    EmbeddedFontSpec classifyEmbeddedFont(const std::string& fontName, ImGuiIO& io) {
        if (fontName.find("fa-solid") != std::string::npos ||
            fontName.find("fa-regular") != std::string::npos) {
            return {.ranges = kFontAwesomeRanges, .isIconFont = true, .isPrimaryTextFont = false};
        }
        if (fontName.find("forkawesome") != std::string::npos) {
            return {.ranges = kForkAwesomeRanges, .isIconFont = true, .isPrimaryTextFont = false};
        }
        if (fontName.find("Cyrillic") != std::string::npos) {
            return {.ranges = io.Fonts->GetGlyphRangesCyrillic(),
                    .isIconFont = false,
                    .isPrimaryTextFont = false};
        }
        return {.ranges = io.Fonts->GetGlyphRangesDefault(),
                .isIconFont = false,
                .isPrimaryTextFont = true};
    }

    void configureEmbeddedFont(ImFontConfig& config, const bool isIconFont) {
        config.FontDataOwnedByAtlas = false;
        if (isIconFont) {
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.PixelSnapH = true;
        }
    }
} // namespace

void fonts::setup() {
    ImGuiIO& io = ImGui::GetIO();

    const size_t embeddedFontCount = getEmbeddedFontCount();
    if (embeddedFontCount == 0)
        return;

    const EmbeddedFont* embeddedFonts = getEmbeddedFonts();
    ImFontConfig fontConfig;

    const EmbeddedFont* primaryFontEntry = nullptr;
    gTabFont = nullptr;

    for (size_t i = 0; i < embeddedFontCount; ++i) {
        const EmbeddedFont& font = embeddedFonts[i];
        if (isMarkdownFace(font.name)) {
            continue;
        }
        std::string fontName = font.name;
        const EmbeddedFontSpec spec = classifyEmbeddedFont(fontName, io);
        if (spec.isPrimaryTextFont && !primaryFontEntry) {
            primaryFontEntry = &font;
        }

        ImFontConfig embeddedFontConfig = fontConfig;
        configureEmbeddedFont(embeddedFontConfig, spec.isIconFont);

        const ImFont* loadedFont = io.Fonts->AddFontFromMemoryTTF(
            (void*)font.data, static_cast<int>(font.size), 16.0f, &embeddedFontConfig, spec.ranges);
        if (!fontConfig.MergeMode) {
            fontConfig.MergeMode = true;
        }

        if (loadedFont) {
            spdlog::debug("loaded embedded font: {}", fontName);
        }
    }

    // sidebar shows user-supplied identifiers (db / schema / table / view names),
    // many of which are CJK on international servers. Merge the system CJK font
    // into the default font too so the sidebar renders them. ~3-4MB atlas growth
    // for the Japanese range, which also covers common Chinese ideographs.
    const std::string cjkPath = findSystemCjkFontPath();
    // one mapping shared by both fonts below: the atlas keeps font bytes for on-demand
    // glyphs, and AddFontFromFileTTF read a resident ~23MB copy per font
    static const std::span<char> cjkData =
        cjkPath.empty() ? std::span<char>{} : mapFontFile(cjkPath);
    auto addCjk = [&](ImFontConfig& config) {
        config.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(cjkData.data(), static_cast<int>(cjkData.size()),
                                              16.0f, &config, io.Fonts->GetGlyphRangesJapanese());
    };
    if (!cjkData.empty()) {
        ImFontConfig defaultCjkConfig;
        defaultCjkConfig.MergeMode = true;
        const ImFont* defaultCjk = addCjk(defaultCjkConfig);
        if (defaultCjk) {
            spdlog::info("merged system cjk font into default font: {}", cjkPath);
        } else {
            spdlog::warn("failed to merge system cjk font into default font at {}", cjkPath);
        }
    }

    // tab font: same monospace base + system CJK font merged in. pushed only
    // inside tab content (sql editor, table viewer) for monospace SQL editing.
    if (primaryFontEntry && !cjkData.empty()) {
        ImFontConfig baseConfig;
        configureEmbeddedFont(baseConfig, false);
        gTabFont = io.Fonts->AddFontFromMemoryTTF((void*)primaryFontEntry->data,
                                                  static_cast<int>(primaryFontEntry->size), 16.0f,
                                                  &baseConfig, io.Fonts->GetGlyphRangesDefault());

        if (gTabFont) {
            for (size_t i = 0; i < embeddedFontCount; ++i) {
                const EmbeddedFont& font = embeddedFonts[i];
                if (&font == primaryFontEntry || isMarkdownFace(font.name)) {
                    continue;
                }

                std::string fontName = font.name;
                const EmbeddedFontSpec spec = classifyEmbeddedFont(fontName, io);

                ImFontConfig mergeConfig;
                mergeConfig.MergeMode = true;
                mergeConfig.DstFont = gTabFont;
                configureEmbeddedFont(mergeConfig, spec.isIconFont);
                io.Fonts->AddFontFromMemoryTTF((void*)font.data, static_cast<int>(font.size), 16.0f,
                                               &mergeConfig, spec.ranges);
            }

            ImFontConfig cjkConfig;
            cjkConfig.MergeMode = true;
            cjkConfig.DstFont = gTabFont;
            const ImFont* cjk = addCjk(cjkConfig);
            if (cjk) {
                spdlog::info("loaded system cjk font: {}", cjkPath);
            } else {
                spdlog::warn("failed to load system cjk font at {}, tab font will lack cjk",
                             cjkPath);
                gTabFont = nullptr;
            }
        }
    } else if (!primaryFontEntry) {
        spdlog::debug("no primary font found, skipping tab font");
    } else {
        spdlog::info("no system cjk font found, tab content will not render cjk");
    }
}

ImFont* fonts::tabFont() {
    return gTabFont;
}
