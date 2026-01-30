#include "database/sql_builder.hpp"
#include <format>

// ========== Factory Function ==========

std::unique_ptr<ISQLBuilder> createSQLBuilder(DatabaseType type) {
    switch (type) {
    case DatabaseType::POSTGRESQL:
        return std::make_unique<PostgreSQLBuilder>();
    case DatabaseType::MYSQL:
        return std::make_unique<MySQLBuilder>();
    case DatabaseType::SQLITE:
        return std::make_unique<SQLiteBuilder>();
    default:
        return std::make_unique<SQLiteBuilder>(); // Default to SQLite
    }
}

// ========== PostgreSQL Implementation ==========

std::string PostgreSQLBuilder::quoteIdentifier(const std::string& identifier) const {
    // PostgreSQL uses double quotes for identifiers
    std::string result = "\"";
    for (char c : identifier) {
        if (c == '"') {
            result += "\"\""; // Escape double quotes by doubling
        } else {
            result += c;
        }
    }
    result += "\"";
    return result;
}

std::string PostgreSQLBuilder::quoteString(const std::string& value) const {
    if (value == "NULL") {
        return "NULL";
    }
    // PostgreSQL uses single quotes for strings
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "''"; // Escape single quotes by doubling
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

std::string PostgreSQLBuilder::selectAll(const std::string& table, int limit, int offset) const {
    return std::format("SELECT * FROM {} LIMIT {} OFFSET {}", quoteIdentifier(table), limit,
                       offset);
}

std::string PostgreSQLBuilder::selectWithFilter(const std::string& table,
                                                const std::string& whereClause,
                                                const std::string& orderBy, int limit,
                                                int offset) const {
    std::string sql = "SELECT * FROM " + quoteIdentifier(table);
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    if (!orderBy.empty()) {
        sql += " ORDER BY " + orderBy;
    }
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    return sql;
}

std::string PostgreSQLBuilder::selectCount(const std::string& table,
                                           const std::string& whereClause) const {
    std::string sql = "SELECT COUNT(*) FROM " + quoteIdentifier(table);
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    return sql;
}

std::string PostgreSQLBuilder::update(
    const std::string& table, const std::string& column, const std::string& newValue,
    const std::vector<std::pair<std::string, std::string>>& whereConditions) const {
    std::string sql = "UPDATE " + quoteIdentifier(table);
    sql += " SET " + quoteIdentifier(column) + " = " + quoteString(newValue);

    if (!whereConditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < whereConditions.size(); ++i) {
            if (i > 0) {
                sql += " AND ";
            }
            const auto& [col, val] = whereConditions[i];
            if (val == "NULL") {
                sql += quoteIdentifier(col) + " IS NULL";
            } else {
                sql += quoteIdentifier(col) + " = " + quoteString(val);
            }
        }
    }
    return sql;
}

std::string PostgreSQLBuilder::insert(const std::string& table,
                                      const std::vector<std::string>& columns,
                                      const std::vector<std::string>& values) const {
    std::string sql = "INSERT INTO " + quoteIdentifier(table) + " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quoteIdentifier(columns[i]);
    }
    sql += ") VALUES (";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quoteString(values[i]);
    }
    sql += ")";
    return sql;
}

std::string PostgreSQLBuilder::deleteFrom(
    const std::string& table,
    const std::vector<std::pair<std::string, std::string>>& whereConditions) const {
    std::string sql = "DELETE FROM " + quoteIdentifier(table);
    if (!whereConditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < whereConditions.size(); ++i) {
            if (i > 0) {
                sql += " AND ";
            }
            const auto& [col, val] = whereConditions[i];
            if (val == "NULL") {
                sql += quoteIdentifier(col) + " IS NULL";
            } else {
                sql += quoteIdentifier(col) + " = " + quoteString(val);
            }
        }
    }
    return sql;
}

std::string PostgreSQLBuilder::createTable(const Table& table) const {
    std::string sql = "CREATE TABLE " + quoteIdentifier(table.name) + " (\n";

    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];
        if (i > 0) {
            sql += ",\n";
        }
        sql += "    " + quoteIdentifier(col.name) + " " + col.type;
        if (col.isPrimaryKey) {
            sql += " PRIMARY KEY";
        }
        if (col.isNotNull && !col.isPrimaryKey) {
            sql += " NOT NULL";
        }
    }

    sql += "\n)";
    return sql;
}

std::string PostgreSQLBuilder::dropTable(const std::string& table, bool ifExists) const {
    if (ifExists) {
        return "DROP TABLE IF EXISTS " + quoteIdentifier(table);
    }
    return "DROP TABLE " + quoteIdentifier(table);
}

std::string PostgreSQLBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + column.type;
    if (column.isNotNull) {
        sql += " NOT NULL";
    }
    return sql;
}

std::string PostgreSQLBuilder::dropColumn(const std::string& table,
                                          const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}

// ========== MySQL Implementation ==========

std::string MySQLBuilder::quoteIdentifier(const std::string& identifier) const {
    // MySQL uses backticks for identifiers
    std::string result = "`";
    for (char c : identifier) {
        if (c == '`') {
            result += "``"; // Escape backticks by doubling
        } else {
            result += c;
        }
    }
    result += "`";
    return result;
}

std::string MySQLBuilder::quoteString(const std::string& value) const {
    if (value == "NULL") {
        return "NULL";
    }
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "''";
        } else if (c == '\\') {
            result += "\\\\"; // MySQL also needs backslash escaping
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

std::string MySQLBuilder::selectAll(const std::string& table, int limit, int offset) const {
    return std::format("SELECT * FROM {} LIMIT {} OFFSET {}", quoteIdentifier(table), limit,
                       offset);
}

std::string MySQLBuilder::selectWithFilter(const std::string& table, const std::string& whereClause,
                                           const std::string& orderBy, int limit,
                                           int offset) const {
    std::string sql = "SELECT * FROM " + quoteIdentifier(table);
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    if (!orderBy.empty()) {
        sql += " ORDER BY " + orderBy;
    }
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    return sql;
}

std::string MySQLBuilder::selectCount(const std::string& table,
                                      const std::string& whereClause) const {
    std::string sql = "SELECT COUNT(*) FROM " + quoteIdentifier(table);
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    return sql;
}

std::string MySQLBuilder::update(
    const std::string& table, const std::string& column, const std::string& newValue,
    const std::vector<std::pair<std::string, std::string>>& whereConditions) const {
    std::string sql = "UPDATE " + quoteIdentifier(table);
    sql += " SET " + quoteIdentifier(column) + " = " + quoteString(newValue);

    if (!whereConditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < whereConditions.size(); ++i) {
            if (i > 0) {
                sql += " AND ";
            }
            const auto& [col, val] = whereConditions[i];
            if (val == "NULL") {
                sql += quoteIdentifier(col) + " IS NULL";
            } else {
                sql += quoteIdentifier(col) + " = " + quoteString(val);
            }
        }
    }
    return sql;
}

std::string MySQLBuilder::insert(const std::string& table, const std::vector<std::string>& columns,
                                 const std::vector<std::string>& values) const {
    std::string sql = "INSERT INTO " + quoteIdentifier(table) + " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quoteIdentifier(columns[i]);
    }
    sql += ") VALUES (";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quoteString(values[i]);
    }
    sql += ")";
    return sql;
}

std::string MySQLBuilder::deleteFrom(
    const std::string& table,
    const std::vector<std::pair<std::string, std::string>>& whereConditions) const {
    std::string sql = "DELETE FROM " + quoteIdentifier(table);
    if (!whereConditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < whereConditions.size(); ++i) {
            if (i > 0) {
                sql += " AND ";
            }
            const auto& [col, val] = whereConditions[i];
            if (val == "NULL") {
                sql += quoteIdentifier(col) + " IS NULL";
            } else {
                sql += quoteIdentifier(col) + " = " + quoteString(val);
            }
        }
    }
    return sql;
}

std::string MySQLBuilder::createTable(const Table& table) const {
    std::string sql = "CREATE TABLE " + quoteIdentifier(table.name) + " (\n";

    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];
        if (i > 0) {
            sql += ",\n";
        }
        sql += "    " + quoteIdentifier(col.name) + " " + col.type;
        if (col.isPrimaryKey) {
            sql += " PRIMARY KEY";
        }
        if (col.isNotNull && !col.isPrimaryKey) {
            sql += " NOT NULL";
        }
    }

    sql += "\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    return sql;
}

std::string MySQLBuilder::dropTable(const std::string& table, bool ifExists) const {
    if (ifExists) {
        return "DROP TABLE IF EXISTS " + quoteIdentifier(table);
    }
    return "DROP TABLE " + quoteIdentifier(table);
}

std::string MySQLBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + column.type;
    if (column.isNotNull) {
        sql += " NOT NULL";
    }
    return sql;
}

std::string MySQLBuilder::dropColumn(const std::string& table,
                                     const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}

// ========== SQLite Implementation ==========

std::string SQLiteBuilder::quoteIdentifier(const std::string& identifier) const {
    // SQLite uses double quotes for identifiers (same as standard SQL)
    std::string result = "\"";
    for (char c : identifier) {
        if (c == '"') {
            result += "\"\"";
        } else {
            result += c;
        }
    }
    result += "\"";
    return result;
}

std::string SQLiteBuilder::quoteString(const std::string& value) const {
    if (value == "NULL") {
        return "NULL";
    }
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "''";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

std::string SQLiteBuilder::selectAll(const std::string& table, int limit, int offset) const {
    return std::format("SELECT * FROM {} LIMIT {} OFFSET {}", quoteIdentifier(table), limit,
                       offset);
}

std::string SQLiteBuilder::selectWithFilter(const std::string& table,
                                            const std::string& whereClause,
                                            const std::string& orderBy, int limit,
                                            int offset) const {
    std::string sql = "SELECT * FROM " + quoteIdentifier(table);
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    if (!orderBy.empty()) {
        sql += " ORDER BY " + orderBy;
    }
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    return sql;
}

std::string SQLiteBuilder::selectCount(const std::string& table,
                                       const std::string& whereClause) const {
    std::string sql = "SELECT COUNT(*) FROM " + quoteIdentifier(table);
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    return sql;
}

std::string SQLiteBuilder::update(
    const std::string& table, const std::string& column, const std::string& newValue,
    const std::vector<std::pair<std::string, std::string>>& whereConditions) const {
    std::string sql = "UPDATE " + quoteIdentifier(table);
    sql += " SET " + quoteIdentifier(column) + " = " + quoteString(newValue);

    if (!whereConditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < whereConditions.size(); ++i) {
            if (i > 0) {
                sql += " AND ";
            }
            const auto& [col, val] = whereConditions[i];
            if (val == "NULL") {
                sql += quoteIdentifier(col) + " IS NULL";
            } else {
                sql += quoteIdentifier(col) + " = " + quoteString(val);
            }
        }
    }
    return sql;
}

std::string SQLiteBuilder::insert(const std::string& table, const std::vector<std::string>& columns,
                                  const std::vector<std::string>& values) const {
    std::string sql = "INSERT INTO " + quoteIdentifier(table) + " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quoteIdentifier(columns[i]);
    }
    sql += ") VALUES (";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quoteString(values[i]);
    }
    sql += ")";
    return sql;
}

std::string SQLiteBuilder::deleteFrom(
    const std::string& table,
    const std::vector<std::pair<std::string, std::string>>& whereConditions) const {
    std::string sql = "DELETE FROM " + quoteIdentifier(table);
    if (!whereConditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < whereConditions.size(); ++i) {
            if (i > 0) {
                sql += " AND ";
            }
            const auto& [col, val] = whereConditions[i];
            if (val == "NULL") {
                sql += quoteIdentifier(col) + " IS NULL";
            } else {
                sql += quoteIdentifier(col) + " = " + quoteString(val);
            }
        }
    }
    return sql;
}

std::string SQLiteBuilder::createTable(const Table& table) const {
    std::string sql = "CREATE TABLE " + quoteIdentifier(table.name) + " (\n";

    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];
        if (i > 0) {
            sql += ",\n";
        }
        sql += "    " + quoteIdentifier(col.name) + " " + col.type;
        if (col.isPrimaryKey) {
            sql += " PRIMARY KEY";
        }
        if (col.isNotNull && !col.isPrimaryKey) {
            sql += " NOT NULL";
        }
    }

    sql += "\n)";
    return sql;
}

std::string SQLiteBuilder::dropTable(const std::string& table, bool ifExists) const {
    if (ifExists) {
        return "DROP TABLE IF EXISTS " + quoteIdentifier(table);
    }
    return "DROP TABLE " + quoteIdentifier(table);
}

std::string SQLiteBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + column.type;
    // SQLite doesn't support NOT NULL for ADD COLUMN without a default
    return sql;
}

std::string SQLiteBuilder::dropColumn(const std::string& table,
                                      const std::string& columnName) const {
    // SQLite 3.35.0+ supports DROP COLUMN
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}
