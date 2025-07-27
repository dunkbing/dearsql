#pragma once

#include "database/db_interface.hpp"
#include <memory>

namespace HierarchyHelpers {
    void renderTableNode(DatabaseInterface *db, int tableIndex);
    void renderViewNode(DatabaseInterface *db, int viewIndex);
    void renderTablesSection(DatabaseInterface *db);
    void renderViewsSection(DatabaseInterface *db);
    void renderRedisHierarchy(std::shared_ptr<DatabaseInterface> db);
} // namespace HierarchyHelpers
