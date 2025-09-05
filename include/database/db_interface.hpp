#pragma once

#include <memory>
#include <string>
#include <vector>

#include "db.hpp"

enum class DatabaseType { SQLITE, POSTGRESQL, MYSQL, REDIS };

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
    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual bool isConnecting() const {
        return false;
    }
    virtual void startConnectionAsync() {}
    virtual void checkConnectionStatusAsync() {}

    // Database info
    [[nodiscard]] virtual const std::string& getName() const = 0;
    [[nodiscard]] virtual const std::string& getConnectionString() const = 0;
    [[nodiscard]] virtual const std::string& getPath() const = 0;
    [[nodiscard]] virtual void* getConnection() const = 0;
    [[nodiscard]] virtual DatabaseType getType() const = 0;

    // Table management
    virtual void refreshTables() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getTables() const = 0;
    virtual std::vector<Table>& getTables() = 0;
    [[nodiscard]] virtual bool areTablesLoaded() const = 0;
    virtual void setTablesLoaded(bool loaded) = 0;
    [[nodiscard]] virtual bool isLoadingTables() const {
        return false;
    }
    virtual void checkTablesStatusAsync() {}

    // View management
    virtual void refreshViews() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getViews() const = 0;
    virtual std::vector<Table>& getViews() = 0;
    [[nodiscard]] virtual bool areViewsLoaded() const = 0;
    virtual void setViewsLoaded(bool loaded) = 0;
    [[nodiscard]] virtual bool isLoadingViews() const {
        return false;
    }
    virtual void checkViewsStatusAsync() {}

    // Sequence management (Postgres)
    virtual void refreshSequences() = 0;
    [[nodiscard]] virtual const std::vector<std::string>& getSequences() const = 0;
    virtual std::vector<std::string>& getSequences() = 0;
    [[nodiscard]] virtual bool areSequencesLoaded() const = 0;
    virtual void setSequencesLoaded(bool loaded) = 0;
    [[nodiscard]] virtual bool isLoadingSequences() const {
        return false;
    }
    virtual void checkSequencesStatusAsync() {}

    // Query execution
    virtual std::string executeQuery(const std::string& query) = 0;
    virtual std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query) = 0;
    virtual std::vector<std::vector<std::string>> getTableData(const std::string& tableName,
                                                               int limit, int offset) = 0;
    virtual std::vector<std::string> getColumnNames(const std::string& tableName) = 0;
    virtual int getRowCount(const std::string& tableName) = 0;

    // Async table data loading (includes metadata + data)
    virtual void startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                         const std::string& whereClause = "") {}
    [[nodiscard]] virtual bool isLoadingTableData(const std::string& tableName) const {
        return false;
    }
    virtual void checkTableDataStatusAsync(const std::string& tableName) {}
    [[nodiscard]] virtual bool hasTableDataResult(const std::string& tableName) const {
        return false;
    }
    virtual std::vector<std::vector<std::string>> getTableDataResult(const std::string& tableName) {
        return {};
    }
    virtual std::vector<std::string> getColumnNamesResult(const std::string& tableName) {
        return {};
    }
    virtual int getRowCountResult(const std::string& tableName) {
        return 0;
    }
    virtual void clearTableDataResult(const std::string& tableName) {}

    // Legacy methods for backward compatibility (use first table or default behavior)
    [[nodiscard]] virtual bool isLoadingTableData() const {
        return false;
    }
    virtual void checkTableDataStatusAsync() {}
    [[nodiscard]] virtual bool hasTableDataResult() const {
        return false;
    }
    virtual std::vector<std::vector<std::string>> getTableDataResult() {
        return {};
    }
    virtual std::vector<std::string> getColumnNamesResult() {
        return {};
    }
    virtual int getRowCountResult() {
        return 0;
    }
    virtual void clearTableDataResult() {}

    // UI state
    [[nodiscard]] virtual bool isExpanded() const = 0;
    virtual void setExpanded(bool expanded) = 0;

    // Connection attempt tracking
    [[nodiscard]] virtual bool hasAttemptedConnection() const = 0;
    virtual void setAttemptedConnection(bool attempted) = 0;
    [[nodiscard]] virtual const std::string& getLastConnectionError() const = 0;
    virtual void setLastConnectionError(const std::string& error) = 0;

protected:
    // Helper methods to be implemented by subclasses
    virtual std::vector<std::string> getTableNames() = 0;
    virtual std::vector<Column> getTableColumns(const std::string& tableName) = 0;
    virtual std::vector<std::string> getViewNames() = 0;
    virtual std::vector<Column> getViewColumns(const std::string& viewName) = 0;
    virtual std::vector<std::string> getSequenceNames() = 0;
};

// Factory for creating database instances
class DatabaseFactory {
public:
    static std::shared_ptr<DatabaseInterface> createDatabase(const DatabaseConnectionInfo& info);
};
