#pragma once

#include "async_helper.hpp"
#include "db.hpp"
#include "ssh_tunnel.hpp"
#include "utils/logger.hpp"
#include <memory>
#include <string>
#include <vector>

enum class DatabaseType { SQLITE, POSTGRESQL, MYSQL, MARIADB, REDIS, MONGODB, MSSQL };

enum class SSHAuthMethod { Password, PrivateKey };

enum class SslMode { Disable, Allow, Prefer, Require, VerifyCA, VerifyFull };

// Forward declarations (defined in db_factory.cpp)
std::string sslModeToString(SslMode mode);
SslMode stringToSslMode(const std::string& str);

struct SSHConfig {
    bool enabled = false;
    std::string host;
    int port = 22;
    std::string username;
    SSHAuthMethod authMethod = SSHAuthMethod::Password;
    std::string password;       // when authMethod == Password
    std::string privateKeyPath; // when authMethod == PrivateKey
};

struct DatabaseConnectionInfo {
    DatabaseType type = DatabaseType::SQLITE;
    std::string name;
    std::string path; // for SQLite file path
    std::string host;
    int port = 5432;
    std::string database;
    std::string username;
    std::string password;
    bool showAllDatabases = false;
    SslMode sslmode = SslMode::Prefer; // SSL mode (all server backends)
    std::string sslCACertPath;         // CA certificate for verify-ca/verify-full
    SSHConfig ssh;

    // Build database-specific connection string
    [[nodiscard]] std::string buildConnectionString(const std::string& dbName = "") const {
        switch (type) {
        case DatabaseType::SQLITE:
            return path;

        case DatabaseType::POSTGRESQL: {
            std::string connStr = "host=" + host + " port=" + std::to_string(port);
            connStr += " connect_timeout=10";

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

            connStr += " sslmode=" + sslModeToString(sslmode);
            if ((sslmode == SslMode::VerifyCA || sslmode == SslMode::VerifyFull) &&
                !sslCACertPath.empty()) {
                connStr += " sslrootcert=" + sslCACertPath;
            }

            return connStr;
        }

        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB: {
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

        case DatabaseType::MONGODB: {
            // mongodb://[username:password@]host[:port][/database]
            std::string connStr = "mongodb://";
            if (!username.empty()) {
                connStr += username;
                if (!password.empty()) {
                    connStr += ":" + password;
                }
                connStr += "@";
            }
            connStr += host + ":" + std::to_string(port);
            if (!dbName.empty()) {
                connStr += "/" + dbName;
            } else if (!database.empty()) {
                connStr += "/" + database;
            }
            // TLS via sslmode
            if (sslmode == SslMode::Require || sslmode == SslMode::VerifyCA ||
                sslmode == SslMode::VerifyFull) {
                connStr += (connStr.find('?') != std::string::npos) ? "&" : "?";
                connStr += "tls=true";
                if (!sslCACertPath.empty())
                    connStr += "&tlsCAFile=" + sslCACertPath;
            }
            return connStr;
        }

        case DatabaseType::MSSQL: {
            // FreeTDS uses host:port format for dbopen()
            return host + ":" + std::to_string(port);
        }

        default:
            return "";
        }
    }
};

struct CreateDatabaseOptions {
    std::string name;
    std::string comment;
    // PostgreSQL
    std::string owner;
    std::string templateDb;
    std::string encoding;
    std::string tablespace;
    // MySQL
    std::string charset;
    std::string collation;
};

/**
 * Abstract base class for all database implementations.
 * Provides both the interface contract and common functionality:
 * - UI state management
 * - Async connection handling
 * - Basic getters/setters
 * - Schema loading patterns (tables, views, sequences)
 */
class DatabaseInterface {
public:
    virtual ~DatabaseInterface() = default;

    // ========== Pure virtual methods (must be implemented by subclasses) ==========

    // Connection management
    virtual std::pair<bool, std::string> connect() = 0;
    virtual void disconnect() = 0;

    // ========== Database operations (with default unsupported implementations) ==========

    // Create a database (optional backend-specific comment/description)
    virtual std::pair<bool, std::string> createDatabase(const std::string& dbName,
                                                        const std::string& comment = "") {
        return {false, "Create database not supported for this database type"};
    }

    // Create a database with extended options (owner, template, encoding, charset, etc.)
    virtual std::pair<bool, std::string>
    createDatabaseWithOptions(const CreateDatabaseOptions& options) {
        return createDatabase(options.name, options.comment);
    }

    // Rename a database (returns success, error message)
    virtual std::pair<bool, std::string> renameDatabase(const std::string& oldName,
                                                        const std::string& newName) {
        return {false, "Rename database not supported for this database type"};
    }

    // Drop a database (returns success, error message)
    virtual std::pair<bool, std::string> dropDatabase(const std::string& dbName) {
        return {false, "Drop database not supported for this database type"};
    }

    // ========== Virtual methods with default implementations ==========

    // Connection status
    [[nodiscard]] virtual bool isConnected() const {
        return connected;
    }

    [[nodiscard]] virtual bool isConnecting() const {
        return connectionOp.isRunning();
    }

    // Refresh connection and all child data
    virtual void refreshConnection() {
        Logger::info("DatabaseInterface: refreshConnection");
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");
        auto [success, error] = connect();
        if (!success) {
            setLastConnectionError(error);
        }
    }

    // Async connection with automatic error handling
    virtual void startConnectionAsync() {
        connectionOp.start([this]() { return this->connect(); });
    }

    virtual void checkConnectionStatusAsync() {
        connectionOp.check([this](std::pair<bool, std::string> result) {
            auto [success, error] = result;
            setAttemptedConnection(true);
            if (!success) {
                setLastConnectionError(error);
            } else {
                setLastConnectionError("");
            }
        });
    }

    // Saved connection ID (for app state persistence)
    virtual void setConnectionId(int id) {
        savedConnectionId = id;
    }

    [[nodiscard]] virtual int getConnectionId() const {
        return savedConnectionId;
    }

    // Connection attempt tracking
    [[nodiscard]] virtual bool hasAttemptedConnection() const {
        return attemptedConnection;
    }

    virtual void setAttemptedConnection(bool attempted) {
        attemptedConnection = attempted;
    }

    [[nodiscard]] virtual const std::string& getLastConnectionError() const {
        return lastConnectionError;
    }

    virtual void setLastConnectionError(const std::string& error) {
        lastConnectionError = error;
    }

    // Connection info getter/setter
    virtual const DatabaseConnectionInfo& getConnectionInfo() const {
        return connectionInfo;
    }

    virtual void setConnectionInfo(const DatabaseConnectionInfo& info) {
        connectionInfo = info;
        if (!info.database.empty())
            return;

        switch (info.type) {
        case DatabaseType::POSTGRESQL: {
            connectionInfo.database = "postgres";
            break;
        }
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB: {
            connectionInfo.database = "mysql";
            break;
        }
        case DatabaseType::MSSQL: {
            connectionInfo.database = "master";
            break;
        }
        default:
            return;
        }
    }

    // Async operation status
    [[nodiscard]] virtual bool hasPendingAsyncWork() const {
        return false;
    }

protected:
    std::pair<bool, std::string> prepareConnectionForConnect() {
        // SSH disabled: ensure any previous tunnel is gone and restore remote endpoint.
        if (!connectionInfo.ssh.enabled) {
            if (sshTunnel_.isRunning())
                sshTunnel_.stop();
            if (sshTunnel_.hasOriginals()) {
                connectionInfo.host = sshTunnel_.remoteHost();
                connectionInfo.port = sshTunnel_.remotePort();
            }
            return {true, ""};
        }

        // If host/port currently points at a previous local tunnel endpoint,
        // recover the original remote endpoint before starting a new tunnel.
        std::string remoteHost = connectionInfo.host;
        int remotePort = connectionInfo.port;
        if (sshTunnel_.hasOriginals() && connectionInfo.host == "127.0.0.1" &&
            connectionInfo.port == sshTunnel_.localPort()) {
            remoteHost = sshTunnel_.remoteHost();
            remotePort = sshTunnel_.remotePort();
        }

        if (remoteHost.empty() || remotePort <= 0 || remotePort > 65535) {
            return {false, "SSH tunnel: invalid remote database host/port"};
        }

        // Always restart to guarantee tunnel settings match current connection info.
        if (sshTunnel_.isRunning())
            sshTunnel_.stop();

        auto [ok, err] = sshTunnel_.start(connectionInfo.ssh, remoteHost, remotePort);
        if (!ok)
            return {false, "SSH tunnel: " + err};

        connectionInfo.host = "127.0.0.1";
        connectionInfo.port = sshTunnel_.localPort();
        return {true, ""};
    }

    void stopSshTunnel() {
        if (sshTunnel_.isRunning())
            sshTunnel_.stop();
        if (sshTunnel_.hasOriginals()) {
            connectionInfo.host = sshTunnel_.remoteHost();
            connectionInfo.port = sshTunnel_.remotePort();
        }
    }

    // Common state
    bool attemptedConnection = false;
    std::string lastConnectionError;
    // Persistent connection ID for app state
    int savedConnectionId = -1;
    bool connected = false;
    DatabaseConnectionInfo connectionInfo;
    SSHTunnel sshTunnel_;

    // Async operations
    AsyncOperation<std::pair<bool, std::string>> connectionOp;
    AsyncOperation<std::vector<Table>> tablesOp;
    AsyncOperation<std::vector<Table>> viewsOp;
    AsyncOperation<std::vector<std::string>> sequencesOp;
};

// Helper functions to convert between DatabaseType enum and strings
std::string databaseTypeToString(DatabaseType type);
DatabaseType stringToDatabaseType(const std::string& typeStr);

// Factory for creating database instances
class DatabaseFactory {
public:
    static std::shared_ptr<DatabaseInterface> createDatabase(const DatabaseConnectionInfo& info);
};
