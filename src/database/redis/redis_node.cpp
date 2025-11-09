#include "database/redis/redis_node.hpp"
#include "database/db.hpp"
#include "database/redis.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <future>

void RedisNode::startKeysLoadAsync() {
    if (loadingKeys.load()) {
        return; // already loading
    }

    loadingKeys = true;
    keysFuture = std::async(std::launch::async, [this]() { return getKeysAsync(); });
}

void RedisNode::checkKeysStatusAsync() {
    if (keysFuture.valid() &&
        keysFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            keyGroups = keysFuture.get();
            Logger::info(
                std::format("Key loading completed. Found {} key groups", keyGroups.size()));
            keysLoaded = true;
            loadingKeys = false;
        } catch (const std::exception& e) {
            Logger::error(std::format("Error in key loading: {}", e.what()));
            lastKeysError = e.what();
            keysLoaded = true;
            loadingKeys = false;
        }
    }
}

std::vector<Table> RedisNode::getKeysAsync() {
    std::vector<Table> result;

    try {
        if (!parentDb || !parentDb->isConnected()) {
            Logger::error("Parent database not connected");
            return result;
        }

        // Load key groups from Redis database
        // Redis groups keys by pattern, so we just get the existing tables
        const auto& tables = parentDb->getTables();
        result = tables;

        Logger::info("Finished loading keys. Total key groups: " + std::to_string(result.size()));
    } catch (const std::exception& e) {
        Logger::error(std::format("Error loading keys: {}", e.what()));
    }

    return result;
}
