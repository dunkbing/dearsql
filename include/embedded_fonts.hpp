#pragma once

#include <cstddef>
#include <cstdint>

struct EmbeddedFont {
    const char *name;
    const uint8_t *data;
    size_t size;
};

extern "C" const EmbeddedFont *getEmbeddedFonts();
extern "C" size_t getEmbeddedFontCount();

const EmbeddedFont *findEmbeddedFont(const char *name);
