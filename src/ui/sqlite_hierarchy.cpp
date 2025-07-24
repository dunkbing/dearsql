#include "ui/sqlite_hierarchy.hpp"
#include "database/sqlite.hpp"
#include "ui/hierarchy_helpers.hpp"

namespace SQLiteHierarchy {
    void renderSQLiteHierarchy(SQLiteDatabase *sqliteDb) {
        HierarchyHelpers::renderTablesSection(sqliteDb);
        HierarchyHelpers::renderViewsSection(sqliteDb);
    }
} // namespace SQLiteHierarchy
