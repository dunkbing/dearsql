#pragma once

#include "app_state.hpp"
#include "database/async_helper.hpp"
#include "database/cassandra/cassandra_database_node.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb/mongodb_database_node.hpp"
#include "database/mssql/mssql_database_node.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/oracle/oracle_database_node.hpp"
#include "database/postgres/postgres_backup.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "utils/database_importer.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

    // Must run every frame, not only while the tree is expanded: a confirmed
    // drop is queued from an Alert callback and would otherwise sit unexecuted
    // until the user next expanded the connection.
    void processPendingDatabaseDrop();

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

    [[nodiscard]] bool isSchemaHidden(const std::string& dbName,
                                      const std::string& schemaName) const {
        const auto it = hiddenSchemas_.find(dbName);
        return it != hiddenSchemas_.end() && it->second.contains(schemaName);
    }

    void setSchemaHidden(const std::string& dbName, const std::string& schemaName, bool hidden) {
        if (hidden) {
            hiddenSchemas_[dbName].insert(schemaName);
        } else {
            const auto it = hiddenSchemas_.find(dbName);
            if (it != hiddenSchemas_.end()) {
                it->second.erase(schemaName);
                if (it->second.empty())
                    hiddenSchemas_.erase(it);
            }
        }
    }

private:
    std::shared_ptr<DatabaseInterface> db;
    std::string pendingEditorOpenDbName_;

    std::set<std::string> hiddenDatabases_;
    std::map<std::string, std::set<std::string>> hiddenSchemas_;

    // multi-selection state
    std::unordered_set<const Table*> selectedTables_;
    const Table* lastAnchorTable_ = nullptr;
    std::vector<const Table*> prevVisibleTables_;
    std::vector<const Table*> currVisibleTables_;
    AsyncOperation<PostgresToolResult> postgresToolOp_;
    std::string postgresToolTitle_;
    std::string postgresToolRefreshDbName_;
    bool postgresToolRefreshDatabaseList_ = false;

    // On macOS and Windows, Alert::show invokes its callbacks synchronously,
    // mid-render. Dropping from one would free the node being rendered and
    // invalidate the iteration, so the request is deferred to the next frame.
    std::optional<std::string> pendingDropDatabase_;

    AsyncOperation<DatabaseImporter::Result> importOp_;
    std::shared_ptr<DatabaseImporter::Progress> importProgress_;
    std::string importDbName_;

    void handleTableClick(const Table* table);
    void renderSchemaFilterBadge(const std::string& dbName, std::vector<std::string> schemaNames,
                                 const ImVec2& nodeMin, const ImVec2& nodeMax,
                                 const void* popupKey);
    void renderMultiSelectMenuContent(ITableDataProvider* provider,
                                      const std::vector<Table>& nodeTables,
                                      std::function<void(const std::string&)> dropOne,
                                      DatabaseType dbType = DatabaseType::SQLITE);
    void checkPostgresToolStatus();
    void checkImportStatus();
    void renderImportProgress();
    void startSqlDumpImport(MySQLDatabaseNode* dbData);
    void renderPostgresBackupRestoreMenus(PostgresDatabaseNode* dbData);
    void startPostgresBackup(PostgresDatabaseNode* dbData, PostgresBackupFormat format,
                             bool includeCreateDatabase, bool noOwner);
    void startPostgresRestore(PostgresDatabaseNode* dbData, bool cleanBeforeRestore,
                              bool createDatabase);

    // Database-specific renderers
    void renderPostgresDatabaseNode(PostgresDatabaseNode* dbData);
    void renderPostgresSchemaNode(const PostgresDatabaseNode* dbData,
                                  PostgresSchemaNode* schemaData);
    void renderMySQLDatabaseNode(MySQLDatabaseNode* dbData);
    void renderMSSQLDatabaseNode(MSSQLDatabaseNode* dbData);
    void renderOracleDatabaseNode(OracleDatabaseNode* dbData);
    void renderMongoDBDatabaseNode(MongoDBDatabaseNode* dbData);
    void renderCassandraDatabaseNode(CassandraDatabaseNode* dbData);
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
    void renderCassandraTableNode(Table& table, CassandraDatabaseNode* dbData);
    void renderCassandraViewNode(Table& view, CassandraDatabaseNode* dbData);
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
