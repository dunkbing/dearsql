#pragma once

#include "app_state.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "database/mssql/mssql_database_node.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/oracle/oracle_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include <functional>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

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

    [[nodiscard]] const std::unordered_set<const Table*>& getSelectedTables() const {
        return selectedTables_;
    }

    [[nodiscard]] bool isDatabaseHidden(const std::string& dbName) const {
        return hiddenDatabases_.contains(dbName);
    }

    void setDatabaseHidden(const std::string& dbName, bool hidden) {
        if (hidden)
            hiddenDatabases_.insert(dbName);
        else
            hiddenDatabases_.erase(dbName);
    }

private:
    std::shared_ptr<DatabaseInterface> db;
    std::string pendingEditorOpenDbName_;

    std::set<std::string> hiddenDatabases_;

    // multi-selection state
    std::unordered_set<const Table*> selectedTables_;
    const Table* lastAnchorTable_ = nullptr;
    std::vector<const Table*> prevVisibleTables_;
    std::vector<const Table*> currVisibleTables_;

    void handleTableClick(const Table* table);
    void renderMultiSelectMenuContent(ITableDataProvider* provider,
                                      const std::vector<Table>& nodeTables,
                                      std::function<void(const std::string&)> dropOne,
                                      DatabaseType dbType = DatabaseType::SQLITE);

    // Database-specific renderers
    void renderPostgresDatabaseNode(PostgresDatabaseNode* dbData);
    void renderPostgresSchemaNode(const PostgresDatabaseNode* dbData,
                                  PostgresSchemaNode* schemaData);
    void renderMySQLDatabaseNode(MySQLDatabaseNode* dbData);
    void renderMSSQLDatabaseNode(MSSQLDatabaseNode* dbData);
    void renderOracleDatabaseNode(OracleDatabaseNode* dbData);
    void renderMongoDBDatabaseNode(MongoDBDatabaseNode* dbData);
    void renderSQLiteNode();

    // Table/view rendering (shared across database types)
    void renderTableNode(Table& table, PostgresSchemaNode* schemaNode);
    void renderViewNode(Table& view, PostgresSchemaNode* schemaNode,
                        bool isMaterializedView = false);
    void renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData);
    void renderMySQLViewNode(Table& view, MySQLDatabaseNode* dbData);
    void renderMSSQLSchemaNode(const MSSQLDatabaseNode* dbData, MSSQLSchemaNode* schemaData);
    void renderMSSQLTableNode(Table& table, MSSQLSchemaNode* schemaData);
    void renderMSSQLViewNode(Table& view, MSSQLSchemaNode* schemaData);
    void renderOracleTableNode(Table& table, OracleDatabaseNode* dbData);
    void renderOracleViewNode(Table& view, OracleDatabaseNode* dbData);
    void renderMongoDBCollectionNode(Table& collection, MongoDBDatabaseNode* dbData);
    void renderSQLiteTableNode(Table& table, SQLiteDatabase* sqliteDb);
    void renderSQLiteViewNode(Table& view, SQLiteDatabase* sqliteDb);

    // Scripts associated with this connection
    void renderQueriesNode();

    // Helper to resolve an IDatabaseNode from a SqlScript's metadata
    IDatabaseNode* resolveNodeForQuery(const SqlScript& script) const;

    // Helper function to render a tree node with icon
    static bool
    renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                           const std::string& icon, ImU32 iconColor,
                           ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                      ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                      ImGuiTreeNodeFlags_FramePadding);
};
