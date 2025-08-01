#pragma once

#include "app_state.hpp"
#include "database/db_interface.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

enum class DialogState {
    TypeSelection,
    PostgreSQLConnection,
    MySQLConnection,
    RedisConnection,
    SavedConnections
};

class DatabaseInterface;

class DatabaseConnectionDialog {
public:
    DatabaseConnectionDialog() = default;
    ~DatabaseConnectionDialog() = default;

    // Main dialog function - call this from UI
    void showDialog();

    // Check if dialog is currently open
    bool isDialogOpen() const {
        return isOpen;
    }

    // Get the result (will be nullptr if dialog cancelled or not completed)
    std::shared_ptr<DatabaseInterface> getResult();

    // Reset dialog state
    void reset();

private:
    // Dialog state
    bool isOpen = false;
    DialogState currentState = DialogState::TypeSelection;
    std::atomic<bool> isConnecting = false;
    std::string errorMessage;

    // Async connection
    std::future<std::pair<std::shared_ptr<DatabaseInterface>, std::string>> connectionFuture;

    // Saved connections
    std::vector<SavedConnection> savedConnections;
    int selectedSavedConnection = -1;

    // Selected database type
    DatabaseType selectedDatabaseType = DatabaseType::SQLITE;

    // PostgreSQL/MySQL connection fields
    char connectionName[256] = "";
    char host[256] = "localhost";
    int port = 5432;
    char database[256] = "";
    char username[256] = "";
    char password[256] = "";
    bool showAllDatabases = false;
    int authType = 0; // 0 = Username & Password, 1 = No Auth

    // Result
    std::shared_ptr<DatabaseInterface> result = nullptr;

    // Dialog rendering functions
    void renderTypeSelection();
    void renderPostgresConnection();
    void renderMySQLConnection();
    void renderRedisConnection();
    void renderSavedConnections();
    void loadSavedConnections();

    // Helper functions
    static std::shared_ptr<DatabaseInterface> createSQLiteDatabase();
    std::shared_ptr<DatabaseInterface> createPostgreSQLDatabase();
    std::shared_ptr<DatabaseInterface> createMySQLDatabase();
    std::shared_ptr<DatabaseInterface> createRedisDatabase();

    // Async connection helpers
    void startAsyncConnection();
    void checkAsyncConnectionStatus();
};
