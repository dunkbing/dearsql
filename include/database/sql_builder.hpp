#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>

[[nodiscard]] std::string autoIncrementClause(DatabaseType type);

[[nodiscard]] bool supportsAutoIncrement(DatabaseType dbType, const std::string& columnType);

class ISQLBuilder {
public:
    virtual ~ISQLBuilder() = default;

    [[nodiscard]] virtual DatabaseType databaseType() const = 0;

    // default: double-quote escaping (PostgreSQL/Oracle/SQLite standard)
    [[nodiscard]] virtual std::string quoteIdentifier(const std::string& identifier) const;

    // quoted "schema.table" if schema is set, else just quoted table name
    [[nodiscard]] virtual std::string qualifiedName(const Table& table) const;

    [[nodiscard]] std::string createTable(const Table& table,
                                          const std::string& schemaPrefix = "") const;
    // qualifiedTable is used verbatim (caller-formatted; e.g. quoteIdentifier or
    // pre-built "schema.table"). Backends apply their own dialect to the column.
    [[nodiscard]] virtual std::string addColumn(const std::string& qualifiedTable,
                                                const Column& column) const = 0;
    [[nodiscard]] virtual std::string dropColumn(const std::string& qualifiedTable,
                                                 const std::string& columnName) const = 0;

    // default: "SELECT * FROM qualified [WHERE ...] [ORDER BY ...] LIMIT N OFFSET M"
    [[nodiscard]] virtual std::string selectAll(const Table& table, const std::string& whereClause,
                                                const std::string& orderByClause, int limit,
                                                int offset) const;
    // "SELECT COUNT(*) FROM qualified [WHERE ...]"
    [[nodiscard]] virtual std::string countRows(const Table& table,
                                                const std::string& whereClause) const;

    // Catalog query returning one row per column, with the column name in result column 0,
    // ordered by column position. Dialect-specific.
    [[nodiscard]] virtual std::string columnNames(const Table& table) const = 0;

    // qualifiedTable: pre-quoted identifier (e.g. from quoteIdentifier or qualifiedName).
    // columnNames: unquoted; the builder applies quoteIdentifier.
    // valueLiterals: pre-formatted SQL literals (numbers, 'strings', NULL, TRUE/FALSE).
    [[nodiscard]] virtual std::string
    insertRow(const std::string& qualifiedTable, const std::vector<std::string>& columnNames,
              const std::vector<std::string>& valueLiterals) const;

    // assignments: pairs of (unquoted column name, pre-formatted SQL literal).
    // whereExpr: pre-built WHERE expression (without the WHERE keyword); empty disallowed.
    [[nodiscard]] virtual std::string
    updateRow(const std::string& qualifiedTable,
              const std::vector<std::pair<std::string, std::string>>& assignments,
              const std::string& whereExpr) const;

    [[nodiscard]] virtual std::string deleteRow(const std::string& qualifiedTable,
                                                const std::string& whereExpr) const;

    // schema may be empty; tableName/newName are unquoted.
    [[nodiscard]] virtual std::string renameTable(const std::string& schema,
                                                  const std::string& oldName,
                                                  const std::string& newName) const;

    [[nodiscard]] virtual std::string dropTable(const std::string& schema,
                                                const std::string& tableName) const;

    [[nodiscard]] virtual std::string truncateTable(const std::string& schema,
                                                    const std::string& tableName) const;

    // qualifiedTable used verbatim. oldColumnName/newColumnName are unquoted.
    [[nodiscard]] virtual std::string renameColumn(const std::string& qualifiedTable,
                                                   const std::string& oldColumnName,
                                                   const std::string& newColumnName) const;

    // Returns full SQL (possibly multiple statements separated by "; ") to apply
    // newColumn to qualifiedTable, modifying the column previously named oldColumnName.
    // Does not handle renaming — call renameColumn first if the column was renamed.
    // Returns empty if the dialect does not support column modification.
    [[nodiscard]] virtual std::string alterColumn(const std::string& qualifiedTable,
                                                  const std::string& oldColumnName,
                                                  const Column& newColumn) const;

protected:
    // helper: "<quotedSchema>.<quotedTable>" if schema is non-empty, else just quoted table
    [[nodiscard]] std::string qualifiedRef(const std::string& schema,
                                           const std::string& tableName) const;
};

std::unique_ptr<ISQLBuilder> createSQLBuilder(DatabaseType type);

class PostgreSQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::POSTGRESQL;
    }

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;

    // Postgres uses TRUNCATE TABLE ONLY to avoid cascading to inheriting tables
    [[nodiscard]] std::string truncateTable(const std::string& schema,
                                            const std::string& tableName) const override;
};

class MySQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::MYSQL;
    }
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;

    // MySQL uses "RENAME TABLE old TO new" instead of "ALTER TABLE ... RENAME"
    [[nodiscard]] std::string renameTable(const std::string& schema, const std::string& oldName,
                                          const std::string& newName) const override;

    // MySQL/MariaDB use "MODIFY COLUMN" with a single full column definition
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class MSSQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::MSSQL;
    }
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;

    // MSSQL uses OFFSET/FETCH NEXT and requires an ORDER BY
    [[nodiscard]] std::string selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int offset) const override;

    // MSSQL renames via sp_rename (string-literal arguments, not bracketed identifiers)
    [[nodiscard]] std::string renameTable(const std::string& schema, const std::string& oldName,
                                          const std::string& newName) const override;

    // sp_rename '<table>.<old>', '<new>', 'COLUMN'
    [[nodiscard]] std::string renameColumn(const std::string& qualifiedTable,
                                           const std::string& oldColumnName,
                                           const std::string& newColumnName) const override;

    // ALTER TABLE ... ALTER COLUMN <name> <type> [NOT NULL] (limited; no defaults inline)
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class OracleBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::ORACLE;
    }

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;

    // Oracle uses OFFSET/FETCH NEXT and requires an ORDER BY
    [[nodiscard]] std::string selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int offset) const override;

    // Oracle drops cascade FK constraints
    [[nodiscard]] std::string dropTable(const std::string& schema,
                                        const std::string& tableName) const override;

    // Oracle uses "MODIFY <name> <type>" (no COLUMN keyword)
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class SQLiteBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::SQLITE;
    }

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;

    // SQLite has no general ALTER COLUMN support
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class DuckDBBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::DUCKDB;
    }

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
};

class CassandraBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::CASSANDRA;
    }

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;

    // CQL has no OFFSET; falls back to LIMIT-only paging.
    [[nodiscard]] std::string selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int offset) const override;

    // Cassandra DROP TABLE always uses IF EXISTS
    [[nodiscard]] std::string dropTable(const std::string& schema,
                                        const std::string& tableName) const override;
};
