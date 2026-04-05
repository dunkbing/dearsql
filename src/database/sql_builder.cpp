#include "database/sql_builder.hpp"
#include "database/ddl_utils.hpp"
#include <format>

std::unique_ptr<ISQLBuilder> createSQLBuilder(DatabaseType type) {
    switch (type) {
    case DatabaseType::REDSHIFT:
    case DatabaseType::POSTGRESQL:
        return std::make_unique<PostgreSQLBuilder>();
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        return std::make_unique<MySQLBuilder>();
    case DatabaseType::MSSQL:
        return std::make_unique<MSSQLBuilder>();
    case DatabaseType::SQLITE:
    default:
        return std::make_unique<SQLiteBuilder>();
    }
}

std::string ISQLBuilder::createTable(const Table& table, const std::string& schemaPrefix) const {
    std::string qualifiedName;
    if (!schemaPrefix.empty())
        qualifiedName =
            std::format("{}.{}", quoteIdentifier(schemaPrefix), quoteIdentifier(table.name));
    else
        qualifiedName = quoteIdentifier(table.name);

    const auto dbType = databaseType();
    const bool isMySQL = (dbType == DatabaseType::MYSQL || dbType == DatabaseType::MARIADB);

    std::string sql = std::format("CREATE TABLE {} (", qualifiedName);

    std::vector<std::string> primaryKeyColumns;

    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];
        if (i > 0)
            sql += ", ";

        sql += std::format("{} {}", quoteIdentifier(col.name), col.type);

        if (col.isNotNull && !col.isPrimaryKey)
            sql += " NOT NULL";

        if (isMySQL && !col.comment.empty())
            sql += std::format(" COMMENT '{}'", ddl_utils::escapeSingleQuotes(col.comment));

        if (col.isPrimaryKey)
            primaryKeyColumns.push_back(col.name);
    }

    if (!primaryKeyColumns.empty()) {
        sql += ", PRIMARY KEY (";
        for (size_t i = 0; i < primaryKeyColumns.size(); ++i) {
            if (i > 0)
                sql += ", ";
            sql += quoteIdentifier(primaryKeyColumns[i]);
        }
        sql += ")";
    }

    sql += ")";

    if (isMySQL)
        sql += " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    return sql;
}

std::string PostgreSQLBuilder::quoteIdentifier(const std::string& identifier) const {
    std::string result = "\"";
    for (char c : identifier) {
        if (c == '"')
            result += "\"\"";
        else
            result += c;
    }
    result += "\"";
    return result;
}

std::string PostgreSQLBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + column.type;
    if (column.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string PostgreSQLBuilder::dropColumn(const std::string& table,
                                          const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}

std::string MySQLBuilder::quoteIdentifier(const std::string& identifier) const {
    std::string result = "`";
    for (char c : identifier) {
        if (c == '`')
            result += "``";
        else
            result += c;
    }
    result += "`";
    return result;
}

std::string MySQLBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + column.type;
    if (column.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string MySQLBuilder::dropColumn(const std::string& table,
                                     const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}

std::string MSSQLBuilder::quoteIdentifier(const std::string& identifier) const {
    std::string result = "[";
    for (char c : identifier) {
        if (c == ']')
            result += "]]";
        else
            result += c;
    }
    result += "]";
    return result;
}

std::string MSSQLBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD " + quoteIdentifier(column.name) + " " + column.type;
    if (column.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string MSSQLBuilder::dropColumn(const std::string& table,
                                     const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}

std::string SQLiteBuilder::quoteIdentifier(const std::string& identifier) const {
    std::string result = "\"";
    for (char c : identifier) {
        if (c == '"')
            result += "\"\"";
        else
            result += c;
    }
    result += "\"";
    return result;
}

std::string SQLiteBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + column.type;
    return sql;
}

std::string SQLiteBuilder::dropColumn(const std::string& table,
                                      const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}
