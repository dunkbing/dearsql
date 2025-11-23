#pragma once

#include <memory>
#include <string>
#include <vector>

#include "db.hpp"

enum class DatabaseType { SQLITE, POSTGRESQL, MYSQL, REDIS };

struct DatabaseConnectionInfo {
    DatabaseType type;
    std::string name;
    std::string path; // for SQLite file path
    std::string host;
    int port = 5432;
    std::string database;
    std::string username;
    std::string password;
    bool showAllDatabases = false;

    // Build database-specific connection string
    std::string buildConnectionString(const std::string& dbName = "") const {
        switch (type) {
        case DatabaseType::SQLITE:
            return path;

        case DatabaseType::POSTGRESQL: {
            std::string connStr = "host=" + host + " port=" + std::to_string(port);

            if (!dbName.empty()) {
                connStr += " dbname=" + dbName;
            } else if (!database.empty()) {
                connStr += " dbname=" + database;
            } else {
                connStr += " dbname=postgres";
            }

            if (!username.empty()) {
                connStr += " user=" + username;
            }

            if (!password.empty()) {
                connStr += " password=" + password;
            }

            return connStr;
        }

        case DatabaseType::MYSQL: {
            const std::string targetDb = !dbName.empty() ? dbName : database;
            std::string connStr =
                "host=" + host + " port=" + std::to_string(port) + " dbname=" + targetDb;

            if (!username.empty()) {
                connStr += " user=" + username;
            }

            if (!password.empty()) {
                connStr += " password=" + password;
            }

            return connStr;
        }

        case DatabaseType::REDIS:
            return "redis://" + host + ":" + std::to_string(port);

        default:
            return "";
        }
    }
};

class DatabaseInterface {
public:
    virtual ~DatabaseInterface() = default;

    // Connection management
    virtual std::pair<bool, std::string> connect() = 0;
    virtual void disconnect() = 0;
    virtual void refreshConnection() = 0; // Reconnect and refresh all child data
    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual bool isConnecting() const {
        return false;
    }
    virtual void startConnectionAsync() {}
    virtual void checkConnectionStatusAsync() {}

    // Database info
    [[nodiscard]] virtual DatabaseType getType() const = 0;

    // Saved connection ID (for app state persistence)
    virtual void setConnectionId(int id) {}
    [[nodiscard]] virtual int getConnectionId() const {
        return -1;
    }

    // Table management
    virtual std::vector<Table>& getTables() = 0;

    // View management
    [[nodiscard]] virtual const std::vector<std::string>& getSequences() const = 0;
    virtual std::vector<std::string>& getSequences() = 0;

    // Async operation status
    [[nodiscard]] virtual bool hasPendingAsyncWork() const {
        return false;
    }

    // Query execution
    virtual std::string executeQuery(const std::string& query) = 0;

    // Connection attempt tracking
    [[nodiscard]] virtual bool hasAttemptedConnection() const = 0;
    virtual void setAttemptedConnection(bool attempted) = 0;
    [[nodiscard]] virtual const std::string& getLastConnectionError() const = 0;
    virtual void setLastConnectionError(const std::string& error) = 0;
    virtual const DatabaseConnectionInfo& getConnectionInfo() const = 0;
    virtual void setConnectionInfo(const DatabaseConnectionInfo& info) = 0;
};

// Helper functions to convert between DatabaseType enum and strings
std::string databaseTypeToString(DatabaseType type);
DatabaseType stringToDatabaseType(const std::string& typeStr);

// Factory for creating database instances
class DatabaseFactory {
public:
    static std::shared_ptr<DatabaseInterface> createDatabase(const DatabaseConnectionInfo& info);
};
