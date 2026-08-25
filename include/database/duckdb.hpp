#pragma once

#include "file_database.hpp"
#include <duckdb.h>
#include <mutex>

class DuckDBDatabase final : public FileDatabase {
public:
    DuckDBDatabase(const DatabaseConnectionInfo& connInfo);
    ~DuckDBDatabase() override;

    // csv files are opened as an in-memory duckdb with the file imported as a table
    static bool isCsvPath(const std::string& path);

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;

    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::DUCKDB;
    }

    QueryResult executeQuery(const std::string& sql, int limit = 1000) override;
    std::pair<bool, std::string> createTable(const Table& table) override;

    // Overload without whereClause (internal use)
    std::vector<std::vector<std::string>> getTableData(const std::string& tableName, int limit,
                                                       int offset);
    // IDatabaseNode/ITableDataProvider implementation
    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderBy = "") override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause = "") override;

    void startTableRefreshAsync(const std::string& tableName) override;

    // ========== Internal Methods ==========

    std::vector<Table> getTablesAsync() const override;
    std::vector<Table> getViewsAsync() const override;
    std::vector<std::string> getSequencesAsync() const override;

private:
    struct QueryOutput {
        bool ok = false;
        std::string error;
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        long long affectedRows = 0;
    };

    // rowLimit < 0 means unlimited
    QueryOutput runQuery(const std::string& sql, int rowLimit = -1) const;
    Table loadTableMeta(const std::string& tableName) const;

    duckdb_database db_ = nullptr;
    duckdb_connection con_ = nullptr;
    // ponytail: one shared connection guarded by a mutex; per-thread connections
    // (duckdb_connect is cheap) if concurrent loaders ever contend
    mutable std::mutex queryMutex_;
};
