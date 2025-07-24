#pragma once

#include "database/db_interface.hpp"

namespace HierarchyHelpers {
    void renderTableNode(DatabaseInterface *db, int tableIndex);
    void renderViewNode(DatabaseInterface *db, int viewIndex);
    void renderTablesSection(DatabaseInterface *db);
    void renderViewsSection(DatabaseInterface *db);
} // namespace HierarchyHelpers
