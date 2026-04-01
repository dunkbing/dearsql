#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "embedded_images.hpp"
#include "platform/platform_interface.hpp"
#include "utils/texture_manager.hpp"
#include <spdlog/spdlog.h>

TextureManager& TextureManager::instance() {
    static TextureManager mgr;
    return mgr;
}

void TextureManager::loadDatabaseIcons(PlatformInterface* platform) {
    if (!platform) {
        return;
    }

    struct IconMapping {
        DatabaseType type;
        const char* imageName;
    };

    static constexpr IconMapping mappings[] = {
        {DatabaseType::SQLITE, "sqlite"},     {DatabaseType::POSTGRESQL, "postgresql"},
        {DatabaseType::MYSQL, "mysql"},       {DatabaseType::MARIADB, "mariadb"},
        {DatabaseType::MONGODB, "mongodb"},   {DatabaseType::REDIS, "redis"},
        {DatabaseType::MSSQL, "mssql"},       {DatabaseType::ORACLE, "oracle"},
        {DatabaseType::REDSHIFT, "redshift"},
    };

    for (const auto& [type, name] : mappings) {
        auto tex = loadFromEmbedded(platform, name);
        if (tex != ImTextureID{}) {
            icons_[type] = tex;
        }
    }
}

ImTextureID TextureManager::getIcon(DatabaseType type) const {
    auto it = icons_.find(type);
    return it != icons_.end() ? it->second : ImTextureID{};
}

ImTextureID TextureManager::loadFromEmbedded(PlatformInterface* platform, const char* name) {
    const EmbeddedImage* img = findEmbeddedImage(name);
    if (!img) {
        spdlog::debug("no embedded image for '{}'", name);
        return ImTextureID{};
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(img->data, static_cast<int>(img->size), &w, &h, &channels, 4);
    if (!pixels) {
        spdlog::error("failed to decode image '{}'", name);
        return ImTextureID{};
    }

    ImTextureID tex = platform->createTextureFromRGBA(pixels, w, h);
    stbi_image_free(pixels);

    if (tex != ImTextureID{}) {
        spdlog::debug("loaded icon '{}' ({}x{})", name, w, h);
    }
    return tex;
}
