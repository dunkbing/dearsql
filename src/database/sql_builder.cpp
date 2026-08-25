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
    if (dbType == DatabaseType::DUCKDB) {
        return false; // no auto-increment keyword; duckdb uses sequences
    }
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
    case DatabaseType::CASSANDRA:
        return std::make_unique<CassandraBuilder>();
    case DatabaseType::DUCKDB:
        return std::make_unique<DuckDBBuilder>();
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

        // CQL does not allow NOT NULL in CREATE TABLE
        if (col.isNotNull && !col.isPrimaryKey && dbType != DatabaseType::CASSANDRA)
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

std::string ISQLBuilder::insertRow(const std::string& qualifiedTable,
                                   const std::vector<std::string>& columnNames,
                                   const std::vector<std::string>& valueLiterals) const {
    std::string cols;
    std::string vals;
    const size_t n = std::min(columnNames.size(), valueLiterals.size());
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
            cols += ", ";
            vals += ", ";
        }
        cols += quoteIdentifier(columnNames[i]);
        vals += valueLiterals[i];
    }
    return std::format("INSERT INTO {} ({}) VALUES ({})", qualifiedTable, cols, vals);
}

std::string
ISQLBuilder::updateRow(const std::string& qualifiedTable,
                       const std::vector<std::pair<std::string, std::string>>& assignments,
                       const std::string& whereExpr) const {
    std::string setClause;
    for (size_t i = 0; i < assignments.size(); ++i) {
        if (i > 0)
            setClause += ", ";
        setClause +=
            std::format("{} = {}", quoteIdentifier(assignments[i].first), assignments[i].second);
    }
    return std::format("UPDATE {} SET {} WHERE {}", qualifiedTable, setClause, whereExpr);
}

std::string ISQLBuilder::deleteRow(const std::string& qualifiedTable,
                                   const std::string& whereExpr) const {
    return std::format("DELETE FROM {} WHERE {}", qualifiedTable, whereExpr);
}

std::string ISQLBuilder::qualifiedRef(const std::string& schema,
                                      const std::string& tableName) const {
    if (schema.empty())
        return quoteIdentifier(tableName);
    return std::format("{}.{}", quoteIdentifier(schema), quoteIdentifier(tableName));
}

std::string ISQLBuilder::renameTable(const std::string& schema, const std::string& oldName,
                                     const std::string& newName) const {
    return std::format("ALTER TABLE {} RENAME TO {}", qualifiedRef(schema, oldName),
                       quoteIdentifier(newName));
}

std::string ISQLBuilder::dropTable(const std::string& schema, const std::string& tableName) const {
    return std::format("DROP TABLE {}", qualifiedRef(schema, tableName));
}

std::string ISQLBuilder::truncateTable(const std::string& schema,
                                       const std::string& tableName) const {
    return std::format("TRUNCATE TABLE {}", qualifiedRef(schema, tableName));
}

std::string MySQLBuilder::renameTable(const std::string& schema, const std::string& oldName,
                                      const std::string& newName) const {
    return std::format("RENAME TABLE {} TO {}", qualifiedRef(schema, oldName),
                       quoteIdentifier(newName));
}

std::string MSSQLBuilder::renameTable(const std::string& schema, const std::string& oldName,
                                      const std::string& newName) const {
    // sp_rename takes string-literal arguments; preserve historical un-bracketed form
    const std::string oldArg = schema.empty() ? oldName : std::format("{}.{}", schema, oldName);
    return std::format("EXEC sp_rename '{}', '{}'", oldArg, newName);
}

std::string OracleBuilder::dropTable(const std::string& schema,
                                     const std::string& tableName) const {
    return std::format("DROP TABLE {} CASCADE CONSTRAINTS", qualifiedRef(schema, tableName));
}

std::string PostgreSQLBuilder::truncateTable(const std::string& schema,
                                             const std::string& tableName) const {
    return std::format("TRUNCATE TABLE ONLY {}", qualifiedRef(schema, tableName));
}

std::string CassandraBuilder::dropTable(const std::string& schema,
                                        const std::string& tableName) const {
    return std::format("DROP TABLE IF EXISTS {}", qualifiedRef(schema, tableName));
}

std::string ISQLBuilder::renameColumn(const std::string& qualifiedTable,
                                      const std::string& oldColumnName,
                                      const std::string& newColumnName) const {
    return std::format("ALTER TABLE {} RENAME COLUMN {} TO {}", qualifiedTable,
                       quoteIdentifier(oldColumnName), quoteIdentifier(newColumnName));
}

std::string ISQLBuilder::alterColumn(const std::string& qualifiedTable,
                                     const std::string& /*oldColumnName*/,
                                     const Column& newColumn) const {
    // Default = PostgreSQL multi-statement form
    const std::string col = quoteIdentifier(newColumn.name);
    std::vector<std::string> statements;
    statements.push_back(
        std::format("ALTER TABLE {} ALTER COLUMN {} TYPE {}", qualifiedTable, col, newColumn.type));
    statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} {}", qualifiedTable, col,
                                     newColumn.isNotNull ? "SET NOT NULL" : "DROP NOT NULL"));
    if (!newColumn.defaultValue.empty()) {
        statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT {}",
                                         qualifiedTable, col, newColumn.defaultValue));
    } else {
        statements.push_back(
            std::format("ALTER TABLE {} ALTER COLUMN {} DROP DEFAULT", qualifiedTable, col));
    }
    if (!newColumn.comment.empty()) {
        statements.push_back(std::format("COMMENT ON COLUMN {}.{} IS '{}'", qualifiedTable, col,
                                         ddl_utils::escapeSingleQuotes(newColumn.comment)));
    }
    std::string sql;
    for (size_t i = 0; i < statements.size(); ++i) {
        if (i > 0)
            sql += "; ";
        sql += statements[i];
    }
    return sql;
}

std::string MSSQLBuilder::renameColumn(const std::string& qualifiedTable,
                                       const std::string& oldColumnName,
                                       const std::string& newColumnName) const {
    return std::format("EXEC sp_rename '{}.{}', '{}', 'COLUMN'", qualifiedTable, oldColumnName,
                       newColumnName);
}

std::string MySQLBuilder::alterColumn(const std::string& qualifiedTable,
                                      const std::string& /*oldColumnName*/,
                                      const Column& newColumn) const {
    std::string sql = std::format("ALTER TABLE {} MODIFY COLUMN {} {}", qualifiedTable,
                                  quoteIdentifier(newColumn.name), newColumn.type);
    if (newColumn.isNotNull)
        sql += " NOT NULL";
    if (newColumn.isAutoIncrement)
        sql += autoIncrementClause(databaseType());
    if (!newColumn.defaultValue.empty())
        sql += " DEFAULT " + newColumn.defaultValue;
    if (!newColumn.comment.empty())
        sql += std::format(" COMMENT '{}'", ddl_utils::escapeSingleQuotes(newColumn.comment));
    return sql;
}

std::string MSSQLBuilder::alterColumn(const std::string& qualifiedTable,
                                      const std::string& /*oldColumnName*/,
                                      const Column& newColumn) const {
    std::string sql = std::format("ALTER TABLE {} ALTER COLUMN {} {}", qualifiedTable,
                                  quoteIdentifier(newColumn.name), newColumn.type);
    if (newColumn.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string OracleBuilder::alterColumn(const std::string& qualifiedTable,
                                       const std::string& /*oldColumnName*/,
                                       const Column& newColumn) const {
    std::string sql = std::format("ALTER TABLE {} MODIFY {} {}", qualifiedTable,
                                  quoteIdentifier(newColumn.name), newColumn.type);
    if (newColumn.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string SQLiteBuilder::alterColumn(const std::string& /*qualifiedTable*/,
                                       const std::string& /*oldColumnName*/,
                                       const Column& /*newColumn*/) const {
    return "-- SQLite doesn't support column modification directly";
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

std::string DuckDBBuilder::addColumn(const std::string& qualifiedTable,
                                     const Column& column) const {
    std::string sql = std::format("ALTER TABLE {} ADD COLUMN {} {}", qualifiedTable,
                                  quoteIdentifier(column.name), column.type);
    if (!column.defaultValue.empty())
        sql += " DEFAULT " + column.defaultValue;
    if (column.isNotNull)
        sql += " NOT NULL";
    return sql;
}

std::string DuckDBBuilder::dropColumn(const std::string& qualifiedTable,
                                      const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable,
                       quoteIdentifier(columnName));
}

std::string DuckDBBuilder::columnNames(const Table& table) const {
    return std::format("SELECT column_name FROM information_schema.columns "
                       "WHERE table_name = '{}' AND table_schema = '{}' "
                       "ORDER BY ordinal_position",
                       ddl_utils::escapeSingleQuotes(table.name),
                       table.schema.empty() ? "main" : ddl_utils::escapeSingleQuotes(table.schema));
}

std::string SQLiteBuilder::columnNames(const Table& table) const {
    // pragma_table_info is a table-valued function; name is in result column 0
    return std::format("SELECT name FROM pragma_table_info('{}')", table.name);
}

std::string PostgreSQLBuilder::addColumn(const std::string& qualifiedTable,
                                         const Column& column) const {
    std::string colType = column.isAutoIncrement ? serialTypeForColumn(column.type) : column.type;
    std::string sql = std::format("ALTER TABLE {} ADD COLUMN {} {}", qualifiedTable,
                                  quoteIdentifier(column.name), colType);
    if (column.isNotNull)
        sql += " NOT NULL";
    if (column.isUnique)
        sql += " UNIQUE";
    if (!column.defaultValue.empty())
        sql += " DEFAULT " + column.defaultValue;
    if (!column.comment.empty()) {
        sql += std::format("; COMMENT ON COLUMN {}.{} IS '{}'", qualifiedTable,
                           quoteIdentifier(column.name),
                           ddl_utils::escapeSingleQuotes(column.comment));
    }
    return sql;
}

std::string PostgreSQLBuilder::dropColumn(const std::string& qualifiedTable,
                                          const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable,
                       quoteIdentifier(columnName));
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

std::string MySQLBuilder::addColumn(const std::string& qualifiedTable, const Column& column) const {
    std::string sql = std::format("ALTER TABLE {} ADD COLUMN {} {}", qualifiedTable,
                                  quoteIdentifier(column.name), column.type);
    if (column.isNotNull)
        sql += " NOT NULL";
    if (column.isAutoIncrement)
        sql += autoIncrementClause(DatabaseType::MYSQL);
    if (column.isUnique)
        sql += " UNIQUE";
    if (!column.defaultValue.empty())
        sql += " DEFAULT " + column.defaultValue;
    if (!column.comment.empty())
        sql += std::format(" COMMENT '{}'", ddl_utils::escapeSingleQuotes(column.comment));
    return sql;
}

std::string MySQLBuilder::dropColumn(const std::string& qualifiedTable,
                                     const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable,
                       quoteIdentifier(columnName));
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

std::string MSSQLBuilder::addColumn(const std::string& qualifiedTable, const Column& column) const {
    std::string sql = std::format("ALTER TABLE {} ADD {} {}", qualifiedTable,
                                  quoteIdentifier(column.name), column.type);
    if (column.isAutoIncrement)
        sql += autoIncrementClause(DatabaseType::MSSQL);
    if (column.isNotNull)
        sql += " NOT NULL";
    if (column.isUnique)
        sql += " UNIQUE";
    if (!column.defaultValue.empty())
        sql += " DEFAULT " + column.defaultValue;
    return sql;
}

std::string MSSQLBuilder::dropColumn(const std::string& qualifiedTable,
                                     const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable,
                       quoteIdentifier(columnName));
}

std::string OracleBuilder::addColumn(const std::string& qualifiedTable,
                                     const Column& column) const {
    std::string sql = std::format("ALTER TABLE {} ADD {} {}", qualifiedTable,
                                  quoteIdentifier(column.name), column.type);
    if (column.isAutoIncrement)
        sql += autoIncrementClause(DatabaseType::ORACLE);
    if (column.isNotNull)
        sql += " NOT NULL";
    if (column.isUnique)
        sql += " UNIQUE";
    if (!column.defaultValue.empty())
        sql += " DEFAULT " + column.defaultValue;
    return sql;
}

std::string OracleBuilder::dropColumn(const std::string& qualifiedTable,
                                      const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable,
                       quoteIdentifier(columnName));
}

std::string SQLiteBuilder::addColumn(const std::string& qualifiedTable,
                                     const Column& column) const {
    std::string sql = std::format("ALTER TABLE {} ADD COLUMN {} {}", qualifiedTable,
                                  quoteIdentifier(column.name), column.type);
    if (column.isNotNull)
        sql += " NOT NULL";
    if (column.isUnique)
        sql += " UNIQUE";
    if (!column.defaultValue.empty())
        sql += " DEFAULT " + column.defaultValue;
    return sql;
}

std::string SQLiteBuilder::dropColumn(const std::string& qualifiedTable,
                                      const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable,
                       quoteIdentifier(columnName));
}

std::string CassandraBuilder::addColumn(const std::string& qualifiedTable,
                                        const Column& column) const {
    return std::format("ALTER TABLE {} ADD {} {}", qualifiedTable, quoteIdentifier(column.name),
                       column.type);
}

std::string CassandraBuilder::dropColumn(const std::string& qualifiedTable,
                                         const std::string& columnName) const {
    return std::format("ALTER TABLE {} DROP {}", qualifiedTable, quoteIdentifier(columnName));
}

std::string CassandraBuilder::columnNames(const Table& table) const {
    const std::string keyspace = table.schema;
    return std::format("SELECT column_name FROM system_schema.columns "
                       "WHERE keyspace_name = '{}' AND table_name = '{}'",
                       keyspace, table.name);
}

std::string CassandraBuilder::selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int /*offset*/) const {
    std::string sql = std::format("SELECT * FROM {}", qualifiedName(table));
    if (!whereClause.empty())
        sql += " WHERE " + whereClause + " ALLOW FILTERING";
    if (!orderByClause.empty())
        sql += " ORDER BY " + orderByClause;
    sql += std::format(" LIMIT {}", limit);
    return sql;
}
