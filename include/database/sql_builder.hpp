#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Interface for database-specific SQL generation
 *
 * This service generates properly quoted and formatted SQL for different
 * database backends (PostgreSQL, MySQL, SQLite).
 */
class ISQLBuilder {
public:
    virtual ~ISQLBuilder() = default;

    // ========== Identifier Quoting ==========

    /**
     * @brief Quote an identifier (table name, column name, etc.)
     * @param identifier The identifier to quote
     * @return Quoted identifier safe for use in SQL
     */
    [[nodiscard]] virtual std::string quoteIdentifier(const std::string& identifier) const = 0;

    /**
     * @brief Quote a string value for use in SQL
     * @param value The string value to quote
     * @return Quoted and escaped string
     */
    [[nodiscard]] virtual std::string quoteString(const std::string& value) const = 0;

    // ========== SELECT Queries ==========

    /**
     * @brief Generate SELECT * with pagination
     * @param table Table name
     * @param limit Max rows to return
     * @param offset Starting row offset
     * @return SELECT SQL statement
     */
    [[nodiscard]] virtual std::string selectAll(const std::string& table, int limit,
                                                int offset) const = 0;

    /**
     * @brief Generate SELECT * with WHERE clause and pagination
     * @param table Table name
     * @param whereClause WHERE condition (without WHERE keyword)
     * @param orderBy ORDER BY clause (without ORDER BY keyword)
     * @param limit Max rows to return
     * @param offset Starting row offset
     * @return SELECT SQL statement
     */
    [[nodiscard]] virtual std::string selectWithFilter(const std::string& table,
                                                       const std::string& whereClause,
                                                       const std::string& orderBy, int limit,
                                                       int offset) const = 0;

    /**
     * @brief Generate SELECT COUNT(*) for a table
     * @param table Table name
     * @param whereClause Optional WHERE condition (without WHERE keyword)
     * @return SELECT COUNT SQL statement
     */
    [[nodiscard]] virtual std::string selectCount(const std::string& table,
                                                  const std::string& whereClause = "") const = 0;

    // ========== DML Statements ==========

    /**
     * @brief Generate UPDATE statement
     * @param table Table name
     * @param column Column to update
     * @param newValue New value (will be quoted)
     * @param whereConditions WHERE conditions as column=value pairs
     * @return UPDATE SQL statement
     */
    [[nodiscard]] virtual std::string
    update(const std::string& table, const std::string& column, const std::string& newValue,
           const std::vector<std::pair<std::string, std::string>>& whereConditions) const = 0;

    /**
     * @brief Generate INSERT statement
     * @param table Table name
     * @param columns Column names
     * @param values Values (will be quoted)
     * @return INSERT SQL statement
     */
    [[nodiscard]] virtual std::string insert(const std::string& table,
                                             const std::vector<std::string>& columns,
                                             const std::vector<std::string>& values) const = 0;

    /**
     * @brief Generate DELETE statement
     * @param table Table name
     * @param whereConditions WHERE conditions as column=value pairs
     * @return DELETE SQL statement
     */
    [[nodiscard]] virtual std::string
    deleteFrom(const std::string& table,
               const std::vector<std::pair<std::string, std::string>>& whereConditions) const = 0;

    // ========== DDL Statements ==========

    /**
     * @brief Generate DROP TABLE statement
     * @param table Table name
     * @param ifExists Add IF EXISTS clause
     * @return DROP TABLE SQL statement
     */
    [[nodiscard]] virtual std::string dropTable(const std::string& table,
                                                bool ifExists = true) const = 0;

    /**
     * @brief Generate ALTER TABLE ADD COLUMN statement
     * @param table Table name
     * @param column Column definition
     * @return ALTER TABLE SQL statement
     */
    [[nodiscard]] virtual std::string addColumn(const std::string& table,
                                                const Column& column) const = 0;

    /**
     * @brief Generate ALTER TABLE DROP COLUMN statement
     * @param table Table name
     * @param columnName Column name to drop
     * @return ALTER TABLE SQL statement
     */
    [[nodiscard]] virtual std::string dropColumn(const std::string& table,
                                                 const std::string& columnName) const = 0;
};

/**
 * @brief Factory function to create database-specific SQL builder
 * @param type Database type
 * @return Unique pointer to SQL builder
 */
std::unique_ptr<ISQLBuilder> createSQLBuilder(DatabaseType type);

// ========== Concrete Implementations ==========

class PostgreSQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;
    [[nodiscard]] std::string quoteString(const std::string& value) const override;

    [[nodiscard]] std::string selectAll(const std::string& table, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string selectWithFilter(const std::string& table,
                                               const std::string& whereClause,
                                               const std::string& orderBy, int limit,
                                               int offset) const override;
    [[nodiscard]] std::string selectCount(const std::string& table,
                                          const std::string& whereClause = "") const override;

    [[nodiscard]] std::string
    update(const std::string& table, const std::string& column, const std::string& newValue,
           const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;
    [[nodiscard]] std::string insert(const std::string& table,
                                     const std::vector<std::string>& columns,
                                     const std::vector<std::string>& values) const override;
    [[nodiscard]] std::string deleteFrom(
        const std::string& table,
        const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;

    [[nodiscard]] std::string dropTable(const std::string& table,
                                        bool ifExists = true) const override;
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
};

class MySQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;
    [[nodiscard]] std::string quoteString(const std::string& value) const override;

    [[nodiscard]] std::string selectAll(const std::string& table, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string selectWithFilter(const std::string& table,
                                               const std::string& whereClause,
                                               const std::string& orderBy, int limit,
                                               int offset) const override;
    [[nodiscard]] std::string selectCount(const std::string& table,
                                          const std::string& whereClause = "") const override;

    [[nodiscard]] std::string
    update(const std::string& table, const std::string& column, const std::string& newValue,
           const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;
    [[nodiscard]] std::string insert(const std::string& table,
                                     const std::vector<std::string>& columns,
                                     const std::vector<std::string>& values) const override;
    [[nodiscard]] std::string deleteFrom(
        const std::string& table,
        const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;

    [[nodiscard]] std::string dropTable(const std::string& table,
                                        bool ifExists = true) const override;
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
};

class MSSQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;
    [[nodiscard]] std::string quoteString(const std::string& value) const override;

    [[nodiscard]] std::string selectAll(const std::string& table, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string selectWithFilter(const std::string& table,
                                               const std::string& whereClause,
                                               const std::string& orderBy, int limit,
                                               int offset) const override;
    [[nodiscard]] std::string selectCount(const std::string& table,
                                          const std::string& whereClause = "") const override;

    [[nodiscard]] std::string
    update(const std::string& table, const std::string& column, const std::string& newValue,
           const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;
    [[nodiscard]] std::string insert(const std::string& table,
                                     const std::vector<std::string>& columns,
                                     const std::vector<std::string>& values) const override;
    [[nodiscard]] std::string deleteFrom(
        const std::string& table,
        const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;

    [[nodiscard]] std::string dropTable(const std::string& table,
                                        bool ifExists = true) const override;
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
};

class SQLiteBuilder : public ISQLBuilder {
public:
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;
    [[nodiscard]] std::string quoteString(const std::string& value) const override;

    [[nodiscard]] std::string selectAll(const std::string& table, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string selectWithFilter(const std::string& table,
                                               const std::string& whereClause,
                                               const std::string& orderBy, int limit,
                                               int offset) const override;
    [[nodiscard]] std::string selectCount(const std::string& table,
                                          const std::string& whereClause = "") const override;

    [[nodiscard]] std::string
    update(const std::string& table, const std::string& column, const std::string& newValue,
           const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;
    [[nodiscard]] std::string insert(const std::string& table,
                                     const std::vector<std::string>& columns,
                                     const std::vector<std::string>& values) const override;
    [[nodiscard]] std::string deleteFrom(
        const std::string& table,
        const std::vector<std::pair<std::string, std::string>>& whereConditions) const override;

    [[nodiscard]] std::string dropTable(const std::string& table,
                                        bool ifExists = true) const override;
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
};
