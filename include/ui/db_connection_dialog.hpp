#pragma once

#include "database/async_helper.hpp"
#include "database/db_interface.hpp"
#include <atomic>
#include <functional>
#include <memory>

enum class DialogState { NewConnection };

enum AuthType { AUTH_USERNAME_PASSWORD = 0, AUTH_NONE = 1 };

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

    // Edit an existing connection
    void editConnection(const std::shared_ptr<DatabaseInterface>& db);

private:
    // Dialog state
    bool isOpen = false;
    DialogState currentState = DialogState::NewConnection;
    std::atomic<bool> isConnecting = false;
    std::string errorMessage;
    std::shared_ptr<DatabaseInterface> editingDatabase = nullptr;
    int editingConnectionId = -1;

    // Async connection
    AsyncOperation<std::pair<std::shared_ptr<DatabaseInterface>, std::string>> connectionOp;

    // Selected database type
    DatabaseType selectedDatabaseType = DatabaseType::SQLITE;

    // SQLite connection fields
    char sqlitePath[512] = "";

    // PostgreSQL/MySQL connection fields
    char connectionName[256] = "";
    char host[256] = "localhost";
    int port = 5432;
    char database[256] = "";
    char username[256] = "";
    char password[256] = "";
    bool showAllDatabases = false;
    int authType = AUTH_USERNAME_PASSWORD;

    // Result
    std::shared_ptr<DatabaseInterface> result = nullptr;

    // Dialog rendering functions
    void renderConnectionDialog();

    // Form rendering helpers
    void renderDatabaseTypeSelector();
    void renderSQLiteFields();
    void renderServerFields(bool showDatabase = true, const char* databaseTooltip = nullptr);
    void renderAuthFields(bool defaultNoAuth = false);
    void renderShowAllDatabasesCheckbox();

    // Helper functions
    static std::shared_ptr<DatabaseInterface> createSQLiteDatabase();
    std::shared_ptr<DatabaseInterface> createPostgreSQLDatabase();
    std::shared_ptr<DatabaseInterface> createMySQLDatabase();
    std::shared_ptr<DatabaseInterface> createMongoDBDatabase();
    std::shared_ptr<DatabaseInterface>
    createSqlDatabase(const std::string& defaultDatabase,
                      const std::function<std::shared_ptr<DatabaseInterface>(
                          const std::string&, const std::string&, int, const std::string&,
                          const std::string&, const std::string&, bool)>& factory);
    std::shared_ptr<DatabaseInterface> createRedisDatabase();

    // Async connection helpers
    void startAsyncConnection();
    void checkAsyncConnectionStatus();
};
