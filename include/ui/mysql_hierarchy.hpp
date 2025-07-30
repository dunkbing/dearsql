#pragma once

#include <string>

class MySQLDatabase;

namespace MySQLHierarchy {
    void renderMySQLHierarchy(MySQLDatabase *mysqlDb);
    void renderSingleDatabaseHierarchy(MySQLDatabase *mysqlDb);
    void renderAllDatabasesHierarchy(MySQLDatabase *mysqlDb);
    void renderTableNode(MySQLDatabase *mysqlDb, int tableIndex);
    void renderViewNode(MySQLDatabase *mysqlDb, int viewIndex);
    void renderCachedTableNode(MySQLDatabase *mysqlDb, const std::string &dbName, int tableIndex);
    void renderCachedViewNode(MySQLDatabase *mysqlDb, const std::string &dbName, int viewIndex);
} // namespace MySQLHierarchy
