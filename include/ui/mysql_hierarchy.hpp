#pragma once

class MySQLDatabase;

namespace MySQLHierarchy {
    void renderMySQLHierarchy(MySQLDatabase *mysqlDb);
    void renderTableNode(MySQLDatabase *mysqlDb, int tableIndex);
    void renderViewNode(MySQLDatabase *mysqlDb, int viewIndex);
} // namespace MySQLHierarchy
