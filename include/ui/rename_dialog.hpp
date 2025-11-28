#pragma once

#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "database/sqlite.hpp"
#include <functional>
#include <string>
#include <variant>

/**
 * @brief Dialog for renaming database objects (singleton)
 *
 * Supports renaming:
 * - Databases (PostgreSQL, MySQL)
 * - Schemas (PostgreSQL)
 * - Tables (all database types)
 */
class RenameDialog {
public:
    enum class ObjectType { Database, Schema, Table };

    using RefreshCallback = std::function<void()>;

    // Singleton access
    static RenameDialog& instance();

    // Delete copy/move
    RenameDialog(const RenameDialog&) = delete;
    RenameDialog& operator=(const RenameDialog&) = delete;
    RenameDialog(RenameDialog&&) = delete;
    RenameDialog& operator=(RenameDialog&&) = delete;

    // Show rename dialog for PostgreSQL database
    void show(PostgresDatabaseNode* dbNode, RefreshCallback onSuccess = nullptr);

    // Show rename dialog for PostgreSQL schema
    void show(PostgresSchemaNode* schemaNode, RefreshCallback onSuccess = nullptr);

    // Show rename dialog for MySQL database
    void show(MySQLDatabaseNode* dbNode, RefreshCallback onSuccess = nullptr);

    // Show rename dialog for PostgreSQL table
    void showForTable(PostgresSchemaNode* schemaNode, const std::string& tableName,
                      RefreshCallback onSuccess = nullptr);

    // Show rename dialog for MySQL table
    void showForTable(MySQLDatabaseNode* dbNode, const std::string& tableName,
                      RefreshCallback onSuccess = nullptr);

    // Show rename dialog for SQLite table
    void showForTable(SQLiteDatabase* sqliteDb, const std::string& tableName,
                      RefreshCallback onSuccess = nullptr);

    // Render the dialog (call from UI loop)
    void render();

    // Check if dialog is open
    bool isOpen() const {
        return isDialogOpen;
    }

private:
    RenameDialog() = default;
    ~RenameDialog() = default;

    bool isDialogOpen = false;
    ObjectType currentObjectType = ObjectType::Table;

    // Target object
    std::variant<std::monostate, PostgresDatabaseNode*, PostgresSchemaNode*, MySQLDatabaseNode*,
                 SQLiteDatabase*>
        targetNode;
    std::string targetName;
    char newNameBuffer[256] = {0};

    RefreshCallback successCallback;
    std::string errorMessage;

    bool executeRename();
    std::string generateRenameSQL();
    std::string getObjectTypeDisplayName() const;
    void reset();
};
