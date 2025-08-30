#pragma once

#include <string>

class MySQLDatabase;

namespace MySQLHierarchy {
    void renderMySQLHierarchy(const std::shared_ptr<MySQLDatabase>& mysqlDb);
    void renderSingleDatabaseHierarchy(const std::shared_ptr<MySQLDatabase>& mysqlDb);
    void renderAllDatabasesHierarchy(const std::shared_ptr<MySQLDatabase>& mysqlDb);
    void renderTableNode(const std::shared_ptr<MySQLDatabase>& mysqlDb, int tableIndex);
    void renderViewNode(const std::shared_ptr<MySQLDatabase>& mysqlDb, int viewIndex);
    void renderCachedTableNode(const std::shared_ptr<MySQLDatabase>& mysqlDb,
                               const std::string& dbName, int tableIndex);
    void renderCachedViewNode(const std::shared_ptr<MySQLDatabase>& mysqlDb,
                              const std::string& dbName, int viewIndex);
} // namespace MySQLHierarchy
