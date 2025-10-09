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
    void renderPostgresDatabaseNode(PostgresDatabase* pgDb, PostgresDatabase::DatabaseData* dbData);

    /**
     * @brief Render a PostgreSQL schema node with tables/views/sequences
     *
     * @param pgDb The PostgreSQL database instance
     * @param dbData The parent database data
     * @param schemaData The schema data to render
     */
    void renderPostgresSchemaNode(PostgresDatabase* pgDb, PostgresDatabase::DatabaseData* dbData,
                                  PostgresDatabase::SchemaData* schemaData);

    /**
     * @brief Render a MySQL database node with its tables/views
     *
     * @param mysqlDb The MySQL database instance
     * @param dbData The database data containing tables/views
     */
    void renderMySQLDatabaseNode(MySQLDatabase* mysqlDb, MySQLDatabase::DatabaseData* dbData);

    /**
     * @brief Render a SQLite database node with its tables/views/sequences
     *
     * @param sqliteDb The SQLite database instance
     * @param dbData The database data containing tables/views/sequences
     */
    void renderSQLiteDatabaseNode(SQLiteDatabase* sqliteDb, SQLiteDatabase::DatabaseData* dbData);

} // namespace NewHierarchy
