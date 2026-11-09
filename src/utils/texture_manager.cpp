#include "utils/texture_manager.hpp"
#include "mydear/embedded.hpp"
#include "mydear/image.hpp"
#include "platform/platform_interface.hpp"
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
        {DatabaseType::SQLITE, "sqlite"},
        {DatabaseType::POSTGRESQL, "postgresql"},
        {DatabaseType::MYSQL, "mysql"},
        {DatabaseType::MARIADB, "mariadb"},
        {DatabaseType::MONGODB, "mongodb"},
        {DatabaseType::REDIS, "redis"},
        {DatabaseType::MSSQL, "mssql"},
        {DatabaseType::ORACLE, "oracle"},
        {DatabaseType::REDSHIFT, "redshift"},
        {DatabaseType::CASSANDRA, "cassandra"},
        {DatabaseType::DUCKDB, "duckdb"},
        {DatabaseType::TIDB, "tidb"},
        {DatabaseType::COCKROACHDB, "cockroachdb"},
        {DatabaseType::S3, "s3"},
        {DatabaseType::SCYLLADB, "scylladb"},
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

    const ImTextureID tex =
        mydear::LoadTexture(img->data, img->size, [platform](const uint8_t* rgba, int w, int h) {
            return platform->createTextureFromRGBA(rgba, w, h);
        });
    if (tex == ImTextureID{})
        spdlog::error("failed to load image '{}'", name);
    return tex;
}
