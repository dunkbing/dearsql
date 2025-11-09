#pragma once

#include "base_database.hpp"
#include <memory>
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

// Forward declaration
class SQLiteDatabaseNode;

class SQLiteDatabase final : public BaseDatabaseImpl {
public:
    SQLiteDatabase(std::string name, std::string path);
    ~SQLiteDatabase() override;

    // Connection management (BaseDatabaseImpl handles async)
    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    // Database info
    const std::string& getName() const override;
    const std::string& getConnectionString() const override;
    const std::string& getPath() const;
    void* getConnection() const override;
    DatabaseType getType() const override;

    // Schema management (BaseDatabaseImpl provides getters/setters)
    void refreshTables() override;
    void refreshViews() override;
    void refreshSequences() override;

    // Query execution
    std::string executeQuery(const std::string& query) override;
    std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string& tableName) override;
    int getRowCount(const std::string& tableName) override;

    // Async table data loading (BaseDatabaseImpl provides implementation)
    void startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                 const std::string& whereClause = "") override;

    // Database node access
    std::shared_ptr<SQLiteDatabaseNode> getDatabaseNode() const;

    // Session access for node (returns raw pointer since SQLite has single session)
    soci::session* getSession() const;

protected:
    std::vector<std::string> getTableNames();
    std::vector<Column> getTableColumns(const std::string& tableName) override;
    std::vector<Index> getTableIndexes(const std::string& tableName) const;
    std::vector<ForeignKey> getTableForeignKeys(const std::string& tableName) const;
    std::vector<std::string> getViewNames() override;
    std::vector<Column> getViewColumns(const std::string& viewName) override;
    std::vector<std::string> getSequenceNames() override;

private:
    // SQLite-specific state (base class handles common state)
    std::string path;
    std::unique_ptr<soci::session> session;

    // Database node (represents the single file)
    std::shared_ptr<SQLiteDatabaseNode> databaseNode;
};
