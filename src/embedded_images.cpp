#include "embedded_images.hpp"
#include <cstring>

const EmbeddedImage* findEmbeddedImage(const char* name) {
    const EmbeddedImage* images = getEmbeddedImages();
    const size_t count = getEmbeddedImageCount();

    if (!images || !name) {
        return nullptr;
    }

    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(images[i].name, name) == 0) {
            return &images[i];
        }
    }

    return nullptr;
}
