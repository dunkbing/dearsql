#pragma once

#include <cstddef>
#include <cstdint>

struct EmbeddedImage {
    const char *name;
    const uint8_t *data;
    size_t size;
};

extern "C" const EmbeddedImage *getEmbeddedImages();
extern "C" size_t getEmbeddedImageCount();

// Helper function to find an image by name
const EmbeddedImage *findEmbeddedImage(const char *name);
