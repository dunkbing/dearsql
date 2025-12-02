#pragma once

#include "database/db_interface.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include <memory>

// Forward declarations
class TableDialog;
class DropColumnDialog;

/**
 * @brief Class for rendering database hierarchy nodes in the sidebar
 *
 * This class holds a reference to a DatabaseInterface and provides methods
 * for rendering the database hierarchy tree.
 */
class DatabaseHierarchy {
public:
    explicit DatabaseHierarchy(std::shared_ptr<DatabaseInterface> dbInterface);

    /**
     * @brief Get the database interface
     */
    [[nodiscard]] std::shared_ptr<DatabaseInterface> getDatabase() const {
        return db;
    }

    /**
     * @brief Render the root database node (expands to show one or multiple databases)
     *
     * For SQLite: shows tables/views directly under the root
     * For MySQL/PostgreSQL with showAllDatabases=false: shows single database with tables/views
     * For MySQL/PostgreSQL with showAllDatabases=true: shows list of databases
     */
    void renderRootNode();

private:
    std::shared_ptr<DatabaseInterface> db;

    // Database-specific renderers
    void renderPostgresDatabaseNode(PostgresDatabaseNode* dbData);
    void renderPostgresSchemaNode(const PostgresDatabaseNode* dbData,
                                  PostgresSchemaNode* schemaData);
    void renderMySQLDatabaseNode(MySQLDatabaseNode* dbData);
    void renderSQLiteNode();

    // Table/view rendering (shared across database types)
    void renderTableNode(Table& table, PostgresSchemaNode* schemaNode,
                         const std::string& databaseName, const std::string& schemaName);
    void renderViewNode(Table& view, PostgresSchemaNode* schemaNode,
                        const std::string& databaseName, const std::string& schemaName);
    void renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData);
    void renderMySQLViewNode(Table& view, MySQLDatabaseNode* dbData);
    void renderSQLiteTableNode(Table& table, SQLiteDatabase* sqliteDb);
    void renderSQLiteViewNode(Table& view, SQLiteDatabase* sqliteDb);

    // Helper function to render a tree node with icon
    static bool
    renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                           const std::string& icon, ImU32 iconColor,
                           ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                      ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                      ImGuiTreeNodeFlags_FramePadding);
};
