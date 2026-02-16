#include "database/ddl_builder.hpp"
#include <format>

std::string DDLBuilder::quoteIdentifier(const std::string& id) const {
    switch (dbType_) {
    case DatabaseType::MYSQL:
        return "`" + id + "`";
    case DatabaseType::POSTGRESQL:
        return "\"" + id + "\"";
    case DatabaseType::SQLITE:
    default:
        return "\"" + id + "\"";
    }
}

std::string DDLBuilder::createTable(const Table& table, const std::string& schemaPrefix) const {
    std::string qualifiedName;
    if (!schemaPrefix.empty()) {
        qualifiedName = quoteIdentifier(schemaPrefix) + "." + quoteIdentifier(table.name);
    } else {
        qualifiedName = quoteIdentifier(table.name);
    }

    std::string sql = "CREATE TABLE " + qualifiedName + " (";

    std::vector<std::string> primaryKeyColumns;

    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];

        if (i > 0) {
            sql += ", ";
        }

        sql += quoteIdentifier(col.name) + " " + col.type;

        if (col.isNotNull && !col.isPrimaryKey) {
            sql += " NOT NULL";
        }

        // MySQL column comments
        if (dbType_ == DatabaseType::MYSQL && !col.comment.empty()) {
            std::string escaped;
            for (char ch : col.comment) {
                if (ch == '\'') {
                    escaped += "''";
                } else {
                    escaped += ch;
                }
            }
            sql += " COMMENT '" + escaped + "'";
        }

        if (col.isPrimaryKey) {
            primaryKeyColumns.push_back(col.name);
        }
    }

    if (!primaryKeyColumns.empty()) {
        sql += ", PRIMARY KEY (";
        for (size_t i = 0; i < primaryKeyColumns.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += quoteIdentifier(primaryKeyColumns[i]);
        }
        sql += ")";
    }

    sql += ")";

    // MySQL engine suffix
    if (dbType_ == DatabaseType::MYSQL) {
        sql += " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    }

    return sql;
}
