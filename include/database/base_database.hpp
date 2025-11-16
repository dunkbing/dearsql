#pragma once

#include "async_helper.hpp"
#include "db.hpp"
#include "db_interface.hpp"
#include "db_ui_state.hpp"
#include "utils/logger.hpp"
#include <future>
#include <soci/connection-pool.h>
#include <soci/mysql/soci-mysql.h>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>
#include <stdexcept>

/**
 * Base implementation class providing common functionality for all database types.
 * Reduces code duplication by implementing shared patterns:
 * - UI state management
 * - Async connection handling
 * - Basic getters/setters
 * - Schema loading patterns (tables, views, sequences)
 */
class BaseDatabaseImpl : public DatabaseInterface {
public:
    ~BaseDatabaseImpl() override = default;

    bool hasAttemptedConnection() const override {
        return uiState.attemptedConnection;
    }
    void setAttemptedConnection(bool attempted) override {
        uiState.attemptedConnection = attempted;
    }

    const std::string& getLastConnectionError() const override {
        return uiState.lastConnectionError;
    }
    void setLastConnectionError(const std::string& error) override {
        uiState.lastConnectionError = error;
    }

    void setConnectionId(int id) override {
        uiState.savedConnectionId = id;
    }
    int getConnectionId() const override {
        return uiState.savedConnectionId;
    }

    // Connection status
    bool isConnected() const override {
        return connected;
    }

    bool isConnecting() const override {
        return connectionOp.isRunning();
    }

    // Refresh connection and all child data
    void refreshConnection() override {
        Logger::info("base_database: refreshConnection");
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");
        auto [success, error] = connect();
        if (!success) {
            setLastConnectionError(error);
        }
    }

    // Async connection with automatic error handling
    void startConnectionAsync() override {
        connectionOp.start([this]() { return this->connect(); });
    }

    void checkConnectionStatusAsync() override {
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

    // Schema loading state (tables)
    bool isLoadingTables() const override {
        return tablesOp.isRunning();
    }
    std::vector<Table>& getTables() override {
        return tables;
    }

    const std::vector<std::string>& getSequences() const override {
        return sequences;
    }
    std::vector<std::string>& getSequences() override {
        return sequences;
    }

    // Connection info getter/setter
    const DatabaseConnectionInfo& getConnectionInfo() const override {
        return connectionInfo;
    }
    void setConnectionInfo(const DatabaseConnectionInfo& info) override {
        connectionInfo = info;
        if (!info.database.empty())
            return;

        switch (info.type) {

        case DatabaseType::POSTGRESQL: {
            connectionInfo.database = "postgres";
        }

        case DatabaseType::MYSQL: {
            connectionInfo.database = "mysql";
        }
        default:
            return;
        }
    }
    DatabaseType getType() const override {
        return connectionInfo.type;
    }

protected:
    // Common state
    DatabaseUIState uiState;
    bool connected = false;
    DatabaseConnectionInfo connectionInfo;

    // Schema data
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;

    // Async operations
    AsyncOperation<std::pair<bool, std::string>> connectionOp;
    AsyncOperation<std::vector<Table>> tablesOp;
    AsyncOperation<std::vector<Table>> viewsOp;
    AsyncOperation<std::vector<std::string>> sequencesOp;

    std::unique_ptr<soci::connection_pool>
    initializeConnectionPool(const DatabaseConnectionInfo& info, size_t poolSize = 2) const {
        if (poolSize == 0) {
            throw std::invalid_argument("poolSize must be greater than zero");
        }

        auto pool = std::make_unique<soci::connection_pool>(poolSize);
        auto* poolPtr = pool.get();
        const auto type = info.type;
        const std::string connStr = info.buildConnectionString();

        std::vector<std::future<void>> connectionFutures;
        connectionFutures.reserve(poolSize);

        for (size_t i = 0; i != poolSize; ++i) {
            connectionFutures.emplace_back(std::async(std::launch::async, [poolPtr, i, connStr,
                                                                           type]() {
                soci::session& session = poolPtr->at(i);
                switch (type) {
                case DatabaseType::POSTGRESQL:
                    session.open(soci::postgresql, connStr);
                    break;
                case DatabaseType::MYSQL:
                    session.open(soci::mysql, connStr);
                    break;
                case DatabaseType::SQLITE:
                    session.open(soci::sqlite3, connStr);
                    break;
                default:
                    throw std::runtime_error("initializeConnectionPool: unsupported database type");
                }
            }));
        }

        for (auto& future : connectionFutures) {
            future.wait();
        }

        return pool;
    }
};
