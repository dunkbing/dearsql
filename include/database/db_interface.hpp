#pragma once

#include <memory>
#include <string>
#include <vector>

#include "db.hpp"

enum class DatabaseType { SQLITE, POSTGRESQL };

struct DatabaseConnectionInfo {
    DatabaseType type;
    std::string name;
    std::string path;
    std::string host;
    int port = 5432;
    std::string database;
    std::string username;
    std::string password;
};

class DatabaseInterface {
public:
    virtual ~DatabaseInterface() = default;

    // Connection management
    virtual std::pair<bool, std::string> connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool isConnecting() const {
        return false;
    }
    virtual void startConnectionAsync() {}
    virtual void checkConnectionStatusAsync() {}

    // Database info
    virtual const std::string &getName() const = 0;
    virtual const std::string &getConnectionString() const = 0;
    virtual const std::string &getPath() const = 0;
    virtual void *getConnection() const = 0;
    virtual DatabaseType getType() const = 0;

    // Table management
    virtual void refreshTables() = 0;
    virtual const std::vector<Table> &getTables() const = 0;
    virtual std::vector<Table> &getTables() = 0;
    virtual bool areTablesLoaded() const = 0;
    virtual void setTablesLoaded(bool loaded) = 0;
    virtual bool isLoadingTables() const {
        return false;
    }
    virtual void checkTablesStatusAsync() {}

    // View management
    virtual void refreshViews() = 0;
    virtual const std::vector<Table> &getViews() const = 0;
    virtual std::vector<Table> &getViews() = 0;
    virtual bool areViewsLoaded() const = 0;
    virtual void setViewsLoaded(bool loaded) = 0;
    virtual bool isLoadingViews() const {
        return false;
    }
    virtual void checkViewsStatusAsync() {}

    // Sequence management (Postgres)
    virtual void refreshSequences() = 0;
    virtual const std::vector<std::string> &getSequences() const = 0;
    virtual std::vector<std::string> &getSequences() = 0;
    virtual bool areSequencesLoaded() const = 0;
    virtual void setSequencesLoaded(bool loaded) = 0;
    virtual bool isLoadingSequences() const {
        return false;
    }
    virtual void checkSequencesStatusAsync() {}

    // Query execution
    virtual std::string executeQuery(const std::string &query) = 0;
    virtual std::vector<std::vector<std::string>> getTableData(const std::string &tableName,
                                                               int limit, int offset) = 0;
    virtual std::vector<std::string> getColumnNames(const std::string &tableName) = 0;
    virtual int getRowCount(const std::string &tableName) = 0;

    // UI state
    virtual bool isExpanded() const = 0;
    virtual void setExpanded(bool expanded) = 0;

    // Connection attempt tracking
    virtual bool hasAttemptedConnection() const = 0;
    virtual void setAttemptedConnection(bool attempted) = 0;
    virtual const std::string &getLastConnectionError() const = 0;
    virtual void setLastConnectionError(const std::string &error) = 0;

protected:
    // Helper methods to be implemented by subclasses
    virtual std::vector<std::string> getTableNames() = 0;
    virtual std::vector<Column> getTableColumns(const std::string &tableName) = 0;
    virtual std::vector<std::string> getViewNames() = 0;
    virtual std::vector<Column> getViewColumns(const std::string &viewName) = 0;
    virtual std::vector<std::string> getSequenceNames() = 0;
};

// Factory for creating database instances
class DatabaseFactory {
public:
    static std::shared_ptr<DatabaseInterface> createDatabase(const DatabaseConnectionInfo &info);
};
