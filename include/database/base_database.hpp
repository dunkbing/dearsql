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
    bool areTablesLoaded() const override {
        return tablesLoaded;
    }
    void setTablesLoaded(bool loaded) override {
        tablesLoaded = loaded;
    }
    bool isLoadingTables() const override {
        return tablesOp.isRunning();
    }
    std::vector<Table>& getTables() override {
        return tables;
    }

    // Schema loading state (views)
    void setViewsLoaded(bool loaded) override {
        viewsLoaded = loaded;
    }
    bool isLoadingViews() const override {
        return viewsOp.isRunning();
    }
    const std::vector<Table>& getViews() const override {
        return views;
    }
    std::vector<Table>& getViews() override {
        return views;
    }

    // Schema loading state (sequences)
    void setSequencesLoaded(bool loaded) override {
        sequencesLoaded = loaded;
    }
    bool isLoadingSequences() const override {
        return sequencesOp.isRunning();
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

    bool tablesLoaded = false;
    bool viewsLoaded = false;
    bool sequencesLoaded = false;

    // Async operations
    AsyncOperation<std::pair<bool, std::string>> connectionOp;
    AsyncOperation<std::vector<Table>> tablesOp;
    AsyncOperation<std::vector<Table>> viewsOp;
    AsyncOperation<std::vector<std::string>> sequencesOp;

    // Table data loader (already a good abstraction)
    TableDataLoader tableDataLoader;
};
