#include "ui/mysql_hierarchy.hpp"
#include "database/mysql.hpp"
#include "ui/hierarchy_helpers.hpp"

namespace MySQLHierarchy {
    void renderMySQLHierarchy(MySQLDatabase *mysqlDb) {
        HierarchyHelpers::renderTablesSection(mysqlDb);
        HierarchyHelpers::renderViewsSection(mysqlDb);
    }
} // namespace MySQLHierarchy
