#pragma once

#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/sqlite.hpp"
#include <memory>

/**
 * @brief Renders database hierarchy nodes
 *
 * This namespace provides functions for rendering the database hierarchy in the sidebar.
 */
namespace NewHierarchy {

    /**
     * @brief Render the root database node (expands to show one or multiple databases)
     *
     * For SQLite: shows tables/views directly under the root
     * For MySQL/PostgreSQL with showAllDatabases=false: shows single database with tables/views
     * For MySQL/PostgreSQL with showAllDatabases=true: shows list of databases
     *
     * @param dbInterface The database interface
     */
    void renderRootDatabaseNode(const std::shared_ptr<DatabaseInterface>& dbInterface);

    /**
     * @brief Render a PostgreSQL database node with its schemas
     *
     * @param pgDb The PostgreSQL database instance
     * @param dbData The database data containing schemas
     */
    void renderPostgresDatabaseNode(PostgresDatabase* pgDb, PostgresDatabaseNode* dbData);

    /**
     * @brief Render a PostgreSQL schema node with tables/views/sequences
     *
     * @param pgDb The PostgreSQL database instance
     * @param dbData The parent database data
     * @param schemaData The schema data to render
     */
    void renderPostgresSchemaNode(PostgresDatabase* pgDb, PostgresDatabaseNode* dbData,
                                  PostgresSchemaNode* schemaData);

    /**
     * @brief Render a MySQL database node with its tables/views
     *
     * @param mysqlDb The MySQL database instance
     * @param dbData The database data containing tables/views
     */
    void renderMySQLDatabaseNode(MySQLDatabase* mysqlDb, MySQLDatabaseNode* dbData);

    /**
     * @brief Render a SQLite database node with its tables/views/sequences
     *
     * @param sqliteDb The SQLite database instance
     * @param dbData The database data containing tables/views/sequences
     */
    void renderSQLiteDatabaseNode(SQLiteDatabase* sqliteDb, SQLiteDatabase::DatabaseData* dbData);

    /**
     * @brief Render a table node with expandable structure (columns, keys, indexes)
     *
     * @param table The table to render
     * @param schemaNode The schema node for PostgreSQL (for creating tabs)
     * @param databaseName The database name (for multi-database scenarios)
     * @param schemaName The schema name (for PostgreSQL)
     */
    void renderTableNode(Table& table, PostgresSchemaNode* schemaNode,
                         const std::string& databaseName, const std::string& schemaName);

    /**
     * @brief Render a view node as a leaf item
     *
     * @param view The view to render
     * @param schemaNode The schema node for PostgreSQL (for creating tabs)
     * @param databaseName The database name (for multi-database scenarios)
     * @param schemaName The schema name (for PostgreSQL)
     */
    void renderViewNode(Table& view, PostgresSchemaNode* schemaNode,
                        const std::string& databaseName, const std::string& schemaName);

    /**
     * @brief Render a MySQL table node as a leaf item with double-click and context menu
     *
     * @param table The table to render
     * @param dbData The database data node
     * @param mysqlDb The MySQL database instance
     */
    void renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData, MySQLDatabase* mysqlDb);

    /**
     * @brief Render a MySQL view node as a leaf item with double-click and context menu
     *
     * @param view The view to render
     * @param dbData The database data node
     * @param mysqlDb The MySQL database instance
     */
    void renderMySQLViewNode(Table& view, MySQLDatabaseNode* dbData, MySQLDatabase* mysqlDb);

} // namespace NewHierarchy
