#include "database/sql_builder.hpp"
#include "database/ddl_utils.hpp"
#include <algorithm>
#include <format>

std::string autoIncrementClause(DatabaseType type) {
    switch (type) {
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        return " AUTO_INCREMENT";
    case DatabaseType::SQLITE:
        return " AUTOINCREMENT";
    case DatabaseType::MSSQL:
        return " IDENTITY(1,1)";
    case DatabaseType::ORACLE:
        return " GENERATED ALWAYS AS IDENTITY";
    default:
        return "";
    }
}

namespace {
    std::string normalizeTypeName(const std::string& type) {
        std::string lower = type;
        std::ranges::transform(lower, lower.begin(), ::tolower);

        const auto parenPos = lower.find('(');
        if (parenPos != std::string::npos) {
            lower = lower.substr(0, parenPos);
        }

        const auto spacePos = lower.find(' ');
        if (spacePos != std::string::npos) {
            lower = lower.substr(0, spacePos);
        }

        const auto first = lower.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return "";
        }
        const auto last = lower.find_last_not_of(" \t");
        return lower.substr(first, last - first + 1);
    }

    bool isIntegerType(const std::string& type) {
        const std::string lower = normalizeTypeName(type);
        return lower == "integer" || lower == "int" || lower == "bigint" || lower == "smallint" ||
               lower == "tinyint" || lower == "mediumint";
    }

    // map integer types to their SERIAL equivalents for PostgreSQL
    std::string serialTypeForColumn(const std::string& type) {
        const std::string lower = normalizeTypeName(type);
        if (lower == "bigint")
            return "BIGSERIAL";
        if (lower == "smallint")
            return "SMALLSERIAL";
        return "SERIAL";
    }
} // namespace

bool supportsAutoIncrement(DatabaseType dbType, const std::string& columnType) {
    if (dbType == DatabaseType::SQLITE) {
        const std::string lower = normalizeTypeName(columnType);
        return lower == "integer";
    }
    if (dbType == DatabaseType::ORACLE) {
        const std::string lower = normalizeTypeName(columnType);
        // Oracle uses NUMBER for integer types; also accept standard names
        return lower == "number" || isIntegerType(lower);
    }
    return isIntegerType(columnType);
}

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
    case DatabaseType::ORACLE:
        return std::make_unique<OracleBuilder>();
    case DatabaseType::SQLITE:
    default:
        return std::make_unique<SQLiteBuilder>();
    }
}

// default: double-quote escaping (PostgreSQL/Oracle/SQLite standard)
std::string ISQLBuilder::quoteIdentifier(const std::string& identifier) const {
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

    // SQLite AUTOINCREMENT must be inline and remain the only PRIMARY KEY clause
    std::vector<std::string> trailingPkColumns;
    bool hasInlineSQLitePrimaryKey = false;

    for (size_t i = 0; i < table.columns.size(); ++i) {
        const auto& col = table.columns[i];
        if (i > 0)
            sql += ", ";

        std::string colType = col.type;
        if (col.isAutoIncrement && dbType == DatabaseType::POSTGRESQL)
            colType = serialTypeForColumn(col.type);

        sql += std::format("{} {}", quoteIdentifier(col.name), colType);

        // SQLite: emit PRIMARY KEY inline when AUTOINCREMENT is needed
        bool inlinePk = false;
        if (col.isPrimaryKey && col.isAutoIncrement && dbType == DatabaseType::SQLITE) {
            sql += " PRIMARY KEY AUTOINCREMENT";
            inlinePk = true;
            hasInlineSQLitePrimaryKey = true;
            trailingPkColumns.clear();
        }

        if (col.isNotNull && !col.isPrimaryKey)
            sql += " NOT NULL";

        if (col.isAutoIncrement && dbType != DatabaseType::SQLITE &&
            dbType != DatabaseType::POSTGRESQL)
            sql += autoIncrementClause(dbType);

        if (isMySQL && !col.comment.empty())
            sql += std::format(" COMMENT '{}'", ddl_utils::escapeSingleQuotes(col.comment));

        if (col.isPrimaryKey && !inlinePk &&
            !(dbType == DatabaseType::SQLITE && hasInlineSQLitePrimaryKey))
            trailingPkColumns.push_back(col.name);
    }

    if (!trailingPkColumns.empty()) {
        sql += ", PRIMARY KEY (";
        for (size_t i = 0; i < trailingPkColumns.size(); ++i) {
            if (i > 0)
                sql += ", ";
            sql += quoteIdentifier(trailingPkColumns[i]);
        }
        sql += ")";
    }

    sql += ")";

    if (isMySQL) {
        sql += " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
        if (!table.comment.empty())
            sql += std::format(" COMMENT='{}'", ddl_utils::escapeSingleQuotes(table.comment));
    }

    // PostgreSQL column comments as separate statements
    if (dbType == DatabaseType::POSTGRESQL) {
        for (const auto& col : table.columns) {
            if (!col.comment.empty()) {
                sql += std::format(";\nCOMMENT ON COLUMN {}.{} IS '{}'", qualifiedName,
                                   quoteIdentifier(col.name),
                                   ddl_utils::escapeSingleQuotes(col.comment));
            }
        }
    }

    return sql;
}

std::string ISQLBuilder::qualifiedName(const Table& table) const {
    if (table.schema.empty())
        return quoteIdentifier(table.name);
    return std::format("{}.{}", quoteIdentifier(table.schema), quoteIdentifier(table.name));
}

std::string ISQLBuilder::selectAll(const Table& table, const std::string& whereClause,
                                   const std::string& orderByClause, int limit, int offset) const {
    std::string sql = std::format("SELECT * FROM {}", qualifiedName(table));
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    if (!orderByClause.empty())
        sql += " ORDER BY " + orderByClause;
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    return sql;
}

std::string ISQLBuilder::countRows(const Table& table, const std::string& whereClause) const {
    std::string sql = std::format("SELECT COUNT(*) FROM {}", qualifiedName(table));
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    return sql;
}

std::string MSSQLBuilder::selectAll(const Table& table, const std::string& whereClause,
                                    const std::string& orderByClause, int limit, int offset) const {
    std::string sql = std::format("SELECT * FROM {}", qualifiedName(table));
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    // OFFSET/FETCH NEXT requires ORDER BY; synthesize a stable one if missing
    sql += " ORDER BY " + (orderByClause.empty() ? std::string("(SELECT NULL)") : orderByClause);
    sql += std::format(" OFFSET {} ROWS FETCH NEXT {} ROWS ONLY", offset, limit);
    return sql;
}

std::string OracleBuilder::selectAll(const Table& table, const std::string& whereClause,
                                     const std::string& orderByClause, int limit,
                                     int offset) const {
    std::string sql = std::format("SELECT * FROM {}", qualifiedName(table));
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    sql += " ORDER BY " + (orderByClause.empty() ? std::string("1") : orderByClause);
    sql += std::format(" OFFSET {} ROWS FETCH NEXT {} ROWS ONLY", offset, limit);
    return sql;
}

std::string PostgreSQLBuilder::columnNames(const Table& table) const {
    const std::string schema = table.schema.empty() ? "public" : table.schema;
    return std::format("SELECT a.attname FROM pg_catalog.pg_attribute a "
                       "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
                       "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
                       "WHERE n.nspname = '{}' AND c.relname = '{}' "
                       "AND a.attnum > 0 AND NOT a.attisdropped "
                       "ORDER BY a.attnum",
                       schema, table.name);
}

std::string MySQLBuilder::columnNames(const Table& table) const {
    // DESCRIBE returns name in column 0; schema set via connection's active database
    return std::format("DESCRIBE `{}`", table.name);
}

std::string MSSQLBuilder::columnNames(const Table& table) const {
    return std::format("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
                       "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                       "AND TABLE_NAME = '{}' ORDER BY ORDINAL_POSITION",
                       table.schema, table.name);
}

std::string OracleBuilder::columnNames(const Table& table) const {
    return std::format("SELECT COLUMN_NAME FROM ALL_TAB_COLUMNS "
                       "WHERE OWNER = '{}' AND TABLE_NAME = '{}' ORDER BY COLUMN_ID",
                       table.schema, table.name);
}

std::string SQLiteBuilder::columnNames(const Table& table) const {
    // pragma_table_info is a table-valued function; name is in result column 0
    return std::format("SELECT name FROM pragma_table_info('{}')", table.name);
}

std::string PostgreSQLBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string colType = column.isAutoIncrement ? serialTypeForColumn(column.type) : column.type;
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD COLUMN " + quoteIdentifier(column.name) + " " + colType;
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
    if (column.isAutoIncrement)
        sql += autoIncrementClause(DatabaseType::MYSQL);
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
    if (column.isAutoIncrement)
        sql += autoIncrementClause(DatabaseType::MSSQL);
    if (column.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string MSSQLBuilder::dropColumn(const std::string& table,
                                     const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
}

std::string OracleBuilder::addColumn(const std::string& table, const Column& column) const {
    std::string sql = "ALTER TABLE " + quoteIdentifier(table);
    sql += " ADD " + quoteIdentifier(column.name) + " " + column.type;
    if (column.isAutoIncrement)
        sql += autoIncrementClause(DatabaseType::ORACLE);
    if (column.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string OracleBuilder::dropColumn(const std::string& table,
                                      const std::string& columnName) const {
    return "ALTER TABLE " + quoteIdentifier(table) + " DROP COLUMN " + quoteIdentifier(columnName);
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
