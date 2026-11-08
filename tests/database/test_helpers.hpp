#pragma once

#include "database/db.hpp"
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace TestHelpers {
    inline std::string makeUniqueIdentifier(const std::string_view prefix) {
        static thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<uint64_t> dist;

        std::ostringstream oss;
        oss << prefix << std::hex << dist(rng);
        return oss.str();
    }

    // the postgres, mysql and cassandra suites also run against compatible engines
    // (cockroachdb, tidb, scylladb):
    // `envVar` names the type, `fallback` when unset
    inline DatabaseType typeFromEnv(const char* envVar, DatabaseType fallback) {
        const char* v = std::getenv(envVar);
        return v && *v ? dearsql::stringToDatabaseType(v) : fallback;
    }
} // namespace TestHelpers
