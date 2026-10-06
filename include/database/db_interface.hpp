#pragma once

#include "async_helper.hpp"
#include "db.hpp"
#include "ssh_tunnel.hpp"
#include <atomic>
#include <dearsql/connection_info.hpp>
#include <dearsql/database.hpp>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

using dearsql::CreateDatabaseOptions;
using dearsql::DatabaseType;
using dearsql::databaseTypeToString;
using dearsql::SslMode;
using dearsql::sslModeToString;
using dearsql::stringToDatabaseType;
using dearsql::stringToSslMode;

// single-file backends (connection + node in one class, no host/port/ssl/ssh)
inline bool isFileDatabase(DatabaseType type) {
    return type == DatabaseType::SQLITE || type == DatabaseType::DUCKDB;
}

enum class SSHAuthMethod { Password, PrivateKey };

struct SSHConfig {
    bool enabled = false;
    std::string host;
    int port = 22;
    std::string username;
    SSHAuthMethod authMethod = SSHAuthMethod::Password;
    std::string password;       // when authMethod == Password
    std::string privateKeyPath; // when authMethod == PrivateKey
};

// the library's connection info plus the ssh tunnel, which only the app knows about.
// readOnly (from the lib) is a guard against slips, not a security boundary
struct DatabaseConnectionInfo : dearsql::ConnectionInfo {
    SSHConfig ssh;
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

    // Connection management
    virtual std::pair<bool, std::string> connect() = 0;
    virtual void disconnect() = 0;

    // Database operations
    virtual std::pair<bool, std::string> createDatabase(const std::string& dbName,
                                                        const std::string& comment = "") {
        return {false, "Create database not supported for this database type"};
    }

    virtual std::pair<bool, std::string>
    createDatabaseWithOptions(const CreateDatabaseOptions& options) {
        return createDatabase(options.name, options.comment);
    }

    virtual std::pair<bool, std::string> renameDatabase(const std::string& oldName,
                                                        const std::string& newName) {
        return {false, "Rename database not supported for this database type"};
    }

    virtual std::pair<bool, std::string> dropDatabase(const std::string& dbName) {
        return {false, "Drop database not supported for this database type"};
    }

    virtual void refreshDatabaseNames() {}

    // Connection status
    [[nodiscard]] virtual bool isConnected() const {
        return connected;
    }

    [[nodiscard]] virtual bool isConnecting() const {
        return connectionOp.isRunning();
    }

    // Refresh connection and all child data
    virtual void refreshConnection() {
        spdlog::debug("DatabaseInterface: refreshConnection");
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

    virtual void setConnectionId(int id) {
        savedConnectionId = id;
    }

    [[nodiscard]] virtual int getConnectionId() const {
        return savedConnectionId;
    }

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
        case DatabaseType::REDSHIFT: {
            connectionInfo.database = "dev";
            break;
        }
        case DatabaseType::POSTGRESQL: {
            connectionInfo.database = "postgres";
            break;
        }
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB: {
            // no default: MySQL/MariaDB connect fine without a schema and the
            // user may lack grants on `mysql` (#19)
            break;
        }
        case DatabaseType::MSSQL: {
            connectionInfo.database = "master";
            break;
        }
        case DatabaseType::ORACLE: {
            connectionInfo.database = "ORCL";
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

    // the libdearsql connection behind this one, null when closed (agent tools use it)
    [[nodiscard]] virtual std::shared_ptr<dearsql::IConnection> libConnection() const {
        return nullptr;
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
    std::atomic<bool> connected = false; // written by connect workers, read by the UI
    DatabaseConnectionInfo connectionInfo;
    SSHTunnel sshTunnel_;

    // Async operations
    AsyncOperation<std::pair<bool, std::string>> connectionOp;
    AsyncOperation<std::vector<Table>> tablesOp;
    AsyncOperation<std::vector<Table>> viewsOp;
    AsyncOperation<std::vector<std::string>> sequencesOp;
};

// a loader's result: the list, or the error that stopped it. loaders return it
// and the UI thread applies it, so workers never write state the sidebar reads
template <typename T> struct LoadResult {
    std::vector<T> items;
    std::string error;
};

// run a libdearsql catalog/data call on a loader thread: an error is logged and
// stored in `error` (shown by the sidebar) instead of escaping into check()
template <typename F> auto libCall(std::string& error, const char* what, F&& fn) -> decltype(fn()) {
    try {
        error.clear();
        return fn();
    } catch (const std::exception& e) {
        spdlog::error("{}: {}", what, e.what());
        error = e.what();
        return {};
    }
}

// Factory for creating database instances
class DatabaseFactory {
public:
    static std::shared_ptr<DatabaseInterface> createDatabase(const DatabaseConnectionInfo& info);
};
