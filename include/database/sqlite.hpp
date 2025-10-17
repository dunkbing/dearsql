#pragma once

#include "base_database.hpp"
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

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

    // Per-file data structure (made public for hierarchy rendering)
public:
    /**
     * @brief Database-level data for SQLite
     *
     * SQLite hierarchy: File → Tables/Views/Sequences
     * SQLite is a single-file database, so there's no "databases" layer.
     * This struct holds the contents of the single .db file.
     */
    struct DatabaseData {
        std::string name; // File name/path

        // SQLite: Direct Tables/Views/Sequences (no database or schema layers)
        std::vector<Table> tables;
        std::vector<Table> views;
        std::vector<std::string> sequences; // SQLite has sqlite_sequence

        // Loading state flags
        bool tablesLoaded = false;
        bool viewsLoaded = false;
        bool sequencesLoaded = false;
        std::atomic<bool> loadingTables = false;
        std::atomic<bool> loadingViews = false;
        std::atomic<bool> loadingSequences = false;

        // Async futures
        std::future<std::vector<Table>> tablesFuture;
        std::future<std::vector<Table>> viewsFuture;
        std::future<std::vector<std::string>> sequencesFuture;

        // UI expansion state
        bool tablesExpanded = false;
        bool viewsExpanded = false;
        bool sequencesExpanded = false;

        // Error tracking
        std::string lastTablesError;
        std::string lastViewsError;
        std::string lastSequencesError;
    };

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
};
