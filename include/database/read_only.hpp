#pragma once

#include "database/database_node.hpp"
#include "database/db_interface.hpp"
#include "utils/sql_guard.hpp"
#include <string>

// read-only connections. the flag lives on the connection, so a node reaches it
// through its owning database -- every backend implements ownerDatabase().
//
// ponytail: a guard against slips, not a security boundary. real protection is a
// restricted database user; this stops the app issuing the write.
namespace ReadOnly {

    inline bool isReadOnly(const IDatabaseNode* node) {
        const DatabaseInterface* owner = node ? node->ownerDatabase() : nullptr;
        return owner != nullptr && owner->getConnectionInfo().readOnly;
    }

    // empty when the statement may run, otherwise the reason to show the user
    inline std::string rejectReason(const IDatabaseNode* node, const std::string& sql) {
        if (!isReadOnly(node) || SqlGuard::isReadOnly(sql)) {
            return {};
        }
        return "This connection is read-only. Only SELECT, SHOW, EXPLAIN, DESCRIBE, WITH and "
               "PRAGMA statements can run — uncheck \"Read-only connection\" in Edit connection "
               "to allow writes.";
    }

} // namespace ReadOnly
