#pragma once

#include "db_interface.hpp"
#include <sqlite3.h>

class SQLiteDatabase : public DatabaseInterface {
public:
    SQLiteDatabase(std::string name, std::string path);
    ~SQLiteDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    bool isConnected() const override;

    // Database info
    const std::string &getName() const override;
    const std::string &getConnectionString() const override;
    const std::string &getPath() const override;
    void *getConnection() const override;
    DatabaseType getType() const override;

    // Table management
    void refreshTables() override;
    const std::vector<Table> &getTables() const override;
    std::vector<Table> &getTables() override;
    bool areTablesLoaded() const override;
    void setTablesLoaded(bool loaded) override;

    // Query execution
    std::string executeQuery(const std::string &query) override;
    std::vector<std::vector<std::string>> getTableData(const std::string &tableName, int limit,
                                                       int offset) override;
    std::vector<std::string> getColumnNames(const std::string &tableName) override;
    int getRowCount(const std::string &tableName) override;

    // UI state
    bool isExpanded() const override;
    void setExpanded(bool expanded) override;

    // Connection attempt tracking
    bool hasAttemptedConnection() const override;
    void setAttemptedConnection(bool attempted) override;
    const std::string &getLastConnectionError() const override;
    void setLastConnectionError(const std::string &error) override;

protected:
    std::vector<std::string> getTableNames() override;
    std::vector<Column> getTableColumns(const std::string &tableName) override;

private:
    std::string name;
    std::string path;
    sqlite3 *connection = nullptr;
    std::vector<Table> tables;
    bool connected = false;
    bool expanded = false;
    bool tablesLoaded = false;
    bool attemptedConnection = false;
    std::string lastConnectionError;
};
