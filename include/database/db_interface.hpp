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
    bool showAllDatabases = false;
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
    [[nodiscard]] virtual void* getConnection() const = 0;
    [[nodiscard]] virtual DatabaseType getType() const = 0;

    // Saved connection ID (for app state persistence)
    virtual void setConnectionId(int id) {}
    [[nodiscard]] virtual int getConnectionId() const {
        return -1;
    }

    // Table management
    virtual void refreshTables() = 0;
    virtual std::vector<Table>& getTables() = 0;
    [[nodiscard]] virtual bool areTablesLoaded() const = 0;
    virtual void setTablesLoaded(bool loaded) = 0;
    [[nodiscard]] virtual bool isLoadingTables() const {
        return false;
    }

    // View management
    virtual void refreshViews() = 0;
    [[nodiscard]] virtual const std::vector<Table>& getViews() const = 0;
    virtual std::vector<Table>& getViews() = 0;
    virtual void setViewsLoaded(bool loaded) = 0;
    [[nodiscard]] virtual bool isLoadingViews() const {
        return false;
    }

    // Sequence management (Postgres)
    virtual void refreshSequences() = 0;
    [[nodiscard]] virtual const std::vector<std::string>& getSequences() const = 0;
    virtual std::vector<std::string>& getSequences() = 0;
    virtual void setSequencesLoaded(bool loaded) = 0;
    [[nodiscard]] virtual bool isLoadingSequences() const {
        return false;
    }

    // Query execution
    virtual std::string executeQuery(const std::string& query) = 0;
    virtual std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
    executeQueryStructured(const std::string& query) = 0;
    virtual std::vector<std::vector<std::string>> getTableData(const std::string& tableName,
                                                               int limit, int offset) = 0;
    virtual std::vector<std::string> getColumnNames(const std::string& tableName) = 0;

    // Connection attempt tracking
    [[nodiscard]] virtual bool hasAttemptedConnection() const = 0;
    virtual void setAttemptedConnection(bool attempted) = 0;
    [[nodiscard]] virtual const std::string& getLastConnectionError() const = 0;
    virtual void setLastConnectionError(const std::string& error) = 0;
};

// Helper functions to convert between DatabaseType enum and strings
std::string databaseTypeToString(DatabaseType type);
DatabaseType stringToDatabaseType(const std::string& typeStr);

// Factory for creating database instances
class DatabaseFactory {
public:
    static std::shared_ptr<DatabaseInterface> createDatabase(const DatabaseConnectionInfo& info);
};
