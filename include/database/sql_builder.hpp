#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <memory>
#include <string>

// returns the SQL clause for auto-increment columns (e.g. " AUTO_INCREMENT", " IDENTITY(1,1)")
[[nodiscard]] std::string autoIncrementClause(DatabaseType type);

// returns true if the given column type supports auto-increment for the database
[[nodiscard]] bool supportsAutoIncrement(DatabaseType dbType, const std::string& columnType);

class ISQLBuilder {
public:
    virtual ~ISQLBuilder() = default;

    [[nodiscard]] virtual DatabaseType databaseType() const = 0;

    // default: double-quote escaping (PostgreSQL/Oracle/SQLite standard)
    [[nodiscard]] virtual std::string quoteIdentifier(const std::string& identifier) const;

    [[nodiscard]] std::string createTable(const Table& table,
                                          const std::string& schemaPrefix = "") const;
    [[nodiscard]] virtual std::string addColumn(const std::string& table,
                                                const Column& column) const = 0;
    [[nodiscard]] virtual std::string dropColumn(const std::string& table,
                                                 const std::string& columnName) const = 0;
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
};
