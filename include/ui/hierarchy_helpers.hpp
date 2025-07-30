#pragma once

#include "database/db_interface.hpp"
#include <memory>

namespace HierarchyHelpers {
    void renderTableNode(DatabaseInterface *db, int tableIndex);
    void renderViewNode(DatabaseInterface *db, int viewIndex);
    void renderTablesSection(DatabaseInterface *db);
    void renderViewsSection(DatabaseInterface *db);
    void renderCachedTablesSection(DatabaseInterface *db, const std::string &dbName);
    void renderCachedViewsSection(DatabaseInterface *db, const std::string &dbName);
    void renderCachedTableNode(DatabaseInterface *db, const std::string &dbName, int tableIndex);
    void renderCachedViewNode(DatabaseInterface *db, const std::string &dbName, int viewIndex);
    void renderRedisHierarchy(std::shared_ptr<DatabaseInterface> db);
} // namespace HierarchyHelpers
