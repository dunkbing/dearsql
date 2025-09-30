#pragma once

#include "database/db_interface.hpp"

namespace HierarchyHelpers {
    void renderTableNode(const std::shared_ptr<DatabaseInterface>& db, int tableIndex,
                         const std::string& schemaName = "");
    void renderViewNode(const std::shared_ptr<DatabaseInterface>& db, int viewIndex);
    void renderTablesSection(const std::shared_ptr<DatabaseInterface>& db,
                             const std::string& schemaName = "");
    void renderViewsSection(const std::shared_ptr<DatabaseInterface>& db,
                            const std::string& schemaName = "");
    void renderCachedTablesSection(const std::shared_ptr<DatabaseInterface>& db,
                                   const std::string& dbName);
    void renderCachedViewsSection(const std::shared_ptr<DatabaseInterface>& db,
                                  const std::string& dbName);
    void renderCachedTableNode(const std::shared_ptr<DatabaseInterface>& db,
                               const std::string& dbName, int tableIndex);
    void renderCachedViewNode(const std::shared_ptr<DatabaseInterface>& db,
                              const std::string& dbName, int viewIndex);
    void renderRedisHierarchy(const std::shared_ptr<DatabaseInterface>& db);
} // namespace HierarchyHelpers
