#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <memory>
#include <string>

class ISQLBuilder {
public:
    virtual ~ISQLBuilder() = default;

    [[nodiscard]] virtual DatabaseType databaseType() const = 0;
    [[nodiscard]] virtual std::string quoteIdentifier(const std::string& identifier) const = 0;

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
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;

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

class SQLiteBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::SQLITE;
    }
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;

    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
};
