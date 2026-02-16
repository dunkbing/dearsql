#pragma once

#include "database/db.hpp"
#include "database/db_interface.hpp"
#include <string>

class DDLBuilder {
public:
    explicit DDLBuilder(DatabaseType dbType) : dbType_(dbType) {}

    // Generate CREATE TABLE SQL from a Table definition
    std::string createTable(const Table& table, const std::string& schemaPrefix = "") const;

private:
    DatabaseType dbType_;
    std::string quoteIdentifier(const std::string& id) const;
};
