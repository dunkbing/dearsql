#pragma once

class MySQLDatabase;

namespace MySQLHierarchy {
    void renderMySQLHierarchy(MySQLDatabase *mysqlDb);
    void renderSingleDatabaseHierarchy(MySQLDatabase *mysqlDb);
    void renderAllDatabasesHierarchy(MySQLDatabase *mysqlDb);
    void renderTableNode(MySQLDatabase *mysqlDb, int tableIndex);
    void renderViewNode(MySQLDatabase *mysqlDb, int viewIndex);
} // namespace MySQLHierarchy
