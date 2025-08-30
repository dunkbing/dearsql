#pragma once

#include <cstddef>
#include <cstdint>

struct EmbeddedImage {
    const char* name;
    const uint8_t* data;
    size_t size;
};

extern "C" const EmbeddedImage* getEmbeddedImages();
extern "C" size_t getEmbeddedImageCount();

const EmbeddedImage* findEmbeddedImage(const char* name);
