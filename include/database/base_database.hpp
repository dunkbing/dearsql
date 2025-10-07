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

    // UI state management (common to all DBs)
    bool isExpanded() const override {
        return uiState.expanded;
    }
    void setExpanded(bool expanded) override {
        uiState.expanded = expanded;
    }

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

    void setSavedConnectionId(int id) override {
        uiState.savedConnectionId = id;
    }
    int getSavedConnectionId() const override {
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
    const std::vector<Table>& getTables() const override {
        return tables;
    }
    std::vector<Table>& getTables() override {
        return tables;
    }

    // Schema loading state (views)
    bool areViewsLoaded() const override {
        return viewsLoaded;
    }
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
    bool areSequencesLoaded() const override {
        return sequencesLoaded;
    }
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

    // Async table data loading (using existing TableDataLoader)
    bool isLoadingTableData(const std::string& tableName) const override {
        return tableDataLoader.isLoading(tableName);
    }
    bool hasTableDataResult(const std::string& tableName) const override {
        return tableDataLoader.hasResult(tableName);
    }
    std::vector<std::vector<std::string>>
    getTableDataResult(const std::string& tableName) override {
        return tableDataLoader.getTableData(tableName);
    }
    std::vector<std::string> getColumnNamesResult(const std::string& tableName) override {
        return tableDataLoader.getColumnNames(tableName);
    }
    int getRowCountResult(const std::string& tableName) override {
        return tableDataLoader.getRowCount(tableName);
    }
    void clearTableDataResult(const std::string& tableName) override {
        tableDataLoader.clear(tableName);
    }

    // Legacy single-table methods (backward compatibility)
    bool isLoadingTableData() const override {
        return tableDataLoader.isAnyLoading();
    }
    bool hasTableDataResult() const override {
        return tableDataLoader.hasAnyResult();
    }
    std::vector<std::vector<std::string>> getTableDataResult() override {
        return tableDataLoader.getFirstAvailableTableData();
    }
    std::vector<std::string> getColumnNamesResult() override {
        return tableDataLoader.getFirstAvailableColumnNames();
    }
    int getRowCountResult() override {
        return tableDataLoader.getFirstAvailableRowCount();
    }
    void clearTableDataResult() override {
        tableDataLoader.clearAll();
    }
    void checkTableDataStatusAsync(const std::string& tableName) override {
        tableDataLoader.check(tableName);
    }
    void checkTableDataStatusAsync() override {
        tableDataLoader.checkAll();
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

    // Helper method for checking async schema loading
    void checkTablesStatusAsync() override {
        tablesOp.check([this](std::vector<Table> result) {
            tables = std::move(result);
            tablesLoaded = true;
        });
    }

    void checkViewsStatusAsync() override {
        viewsOp.check([this](std::vector<Table> result) {
            views = std::move(result);
            viewsLoaded = true;
        });
    }

    void checkSequencesStatusAsync() override {
        sequencesOp.check([this](std::vector<std::string> result) {
            sequences = std::move(result);
            sequencesLoaded = true;
        });
    }
};
