#pragma once

#include "database/ddl_utils.hpp"
#include <algorithm>
#include <cctype>
#include <string>

namespace env_tag {
    // Case-insensitive, whitespace-trimmed grouping key. The sidebar groups
    // connections by this, and the connection dialog dedups the tag picker by
    // this — a near-miss under the same rule would silently split a group.
    inline std::string groupKey(const std::string& tag) {
        std::string key = ddl_utils::trim(tag);
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        return key;
    }
} // namespace env_tag
