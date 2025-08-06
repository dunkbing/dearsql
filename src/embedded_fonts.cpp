#include "embedded_fonts.hpp"
#include <cstring>

const EmbeddedFont *findEmbeddedFont(const char *name) {
    const EmbeddedFont *fonts = getEmbeddedFonts();
    const size_t count = getEmbeddedFontCount();

    if (!fonts || !name) {
        return nullptr;
    }

    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(fonts[i].name, name) == 0) {
            return &fonts[i];
        }
    }

    return nullptr;
}
