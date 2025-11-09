#pragma once

#include "database/db.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

// Forward declaration
class RedisDatabase;

/**
 * @brief Node for Redis database hierarchy
 *
 * Redis hierarchy: Connection → Keys (grouped by pattern)
 * Keys are grouped by common prefixes (e.g., "user:", "session:", etc.)
 */
class RedisNode {
public:
    RedisDatabase* parentDb = nullptr;
    std::string name; // Connection name

    // Redis: Key groups (represented as tables)
    std::vector<Table> keyGroups;

    // Loading state flags
    bool keysLoaded = false;
    std::atomic<bool> loadingKeys = false;

    // Async futures
    std::future<std::vector<Table>> keysFuture;

    // UI expansion state
    bool expanded = false;

    // Error tracking
    std::string lastKeysError;

    // Methods
    void startKeysLoadAsync();
    void checkKeysStatusAsync();
    std::vector<Table> getKeysAsync();

    // Getters for compatibility
    const std::vector<Table>& getKeyGroups() const {
        return keyGroups;
    }
};
