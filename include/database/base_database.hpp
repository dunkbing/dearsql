#pragma once

#include "async_helper.hpp"
#include "db.hpp"
#include "db_interface.hpp"
#include "db_ui_state.hpp"

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

protected:
    // Common state
    DatabaseUIState uiState;
    bool connected = false;
    std::string name;

    // Schema data
    std::vector<Table> tables;
    std::vector<Table> views;
    std::vector<std::string> sequences;

    // Async operations
    AsyncOperation<std::pair<bool, std::string>> connectionOp;
    AsyncOperation<std::vector<Table>> tablesOp;
    AsyncOperation<std::vector<Table>> viewsOp;
    AsyncOperation<std::vector<std::string>> sequencesOp;
};
