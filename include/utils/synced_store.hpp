#pragma once

#include <memory>
#include <string>
#include <vector>

// apple only: records kept in core data and, when the app is built with an icloud
// container (DEARSQL_ICLOUD_CONTAINER), mirrored to the user's private cloudkit
// database. one entity, (kind, uuid) -> json, so new fields never need a cloudkit
// schema change. cloudkit has no unique constraints: all() keeps the newest record
// per uuid and deletes the rest
class SyncedStore {
public:
    struct Record {
        std::string uuid;
        std::string json;
    };

    // file: the core data sqlite file. mirror: only one process may mirror to
    // cloudkit (the gui); the terminal modes open the same store without it
    SyncedStore(const std::string& file, bool mirror);
    ~SyncedStore();

    [[nodiscard]] bool ok() const;
    [[nodiscard]] std::vector<Record> all(const std::string& kind) const;
    void put(const std::string& kind, const std::string& uuid, const std::string& json) const;
    void remove(const std::string& kind, const std::string& uuid) const;
    // true once after another device or process changed the store
    bool takeRemoteChange() const;

    static std::string newUuid();
    // whether this build syncs at all (an icloud container was configured)
    static bool syncConfigured();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
