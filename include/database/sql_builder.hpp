#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <memory>
#include <string>

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
    [[nodiscard]] virtual std::string addColumn(const std::string& table,
                                                const Column& column) const = 0;
    [[nodiscard]] virtual std::string dropColumn(const std::string& table,
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
};
