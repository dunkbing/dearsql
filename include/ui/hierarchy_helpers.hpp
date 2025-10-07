#pragma once

#include "database/db_interface.hpp"
#include "imgui.h"

namespace HierarchyHelpers {
    // Shared helper functions
    void renderTreeNodeIcon(const char* icon, const ImVec4& color);
    std::string makeTreeNodeLabel(const std::string& text, const std::string& id = "");
    std::string makeTreeNodeLabel(const std::string& text, const void* objectPtr);
    void renderLoadingState(const char* message, const char* spinnerId);
    void renderDatabaseNodeIcon();

    // Hierarchy rendering
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

    void renderTableLeafItem(const std::shared_ptr<DatabaseInterface>& db, Table& table,
                             const std::string& schemaName = "",
                             const std::string& databaseName = "");
    void renderViewLeafItem(const std::shared_ptr<DatabaseInterface>& db, Table& view,
                            const std::string& schemaName = "",
                            const std::string& databaseName = "");
} // namespace HierarchyHelpers
