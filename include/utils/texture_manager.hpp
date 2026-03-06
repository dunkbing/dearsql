#pragma once

#include "database/db_interface.hpp"
#include "imgui.h"
#include <string>
#include <unordered_map>

class PlatformInterface;

class TextureManager {
public:
    static TextureManager& instance();

    // load all database icon textures from embedded images
    void loadDatabaseIcons(PlatformInterface* platform);

    // get the texture for a database type, returns nullptr if not loaded
    [[nodiscard]] ImTextureID getIcon(DatabaseType type) const;

    // icon dimensions (square)
    [[nodiscard]] float getIconSize() const {
        return iconSize_;
    }

private:
    TextureManager() = default;

    ImTextureID loadFromEmbedded(PlatformInterface* platform, const char* name);

    std::unordered_map<DatabaseType, ImTextureID> icons_;
    float iconSize_ = 14.0f;
};
